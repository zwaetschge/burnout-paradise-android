#include "guest_handles.h"
#include <pthread.h>
#include <stdlib.h>
typedef struct { void *object; GhDestroy destroy; uint32_t refs; } GhObject;
typedef struct { uint32_t kind; GhObject *object; } GhSlot;
static GhSlot slots[GH_MAX_HANDLES];
static uint32_t next_hint;
static pthread_mutex_t lock=PTHREAD_MUTEX_INITIALIZER;
static uint32_t index_of(uint32_t h){
  if(h<GH_FIRST_HANDLE||(h&3u))return GH_MAX_HANDLES;
  uint32_t i=(h-GH_FIRST_HANDLE)>>2; return i<GH_MAX_HANDLES?i:GH_MAX_HANDLES; }
static uint32_t take_slot(uint32_t kind,GhObject *o){
  for(uint32_t n=0;n<GH_MAX_HANDLES;n++){
    uint32_t i=(next_hint+n)%GH_MAX_HANDLES;
    if(slots[i].kind==GH_KIND_NONE){slots[i].kind=kind;slots[i].object=o;
      next_hint=i+1;return GH_FIRST_HANDLE+(i<<2);} }
  return 0; }
uint32_t gh_insert(uint32_t kind,void *object,GhDestroy destroy){
  if(kind==GH_KIND_NONE)return 0;
  GhObject *o=malloc(sizeof *o); if(!o)return 0;
  o->object=object;o->destroy=destroy;o->refs=1;
  pthread_mutex_lock(&lock); uint32_t h=take_slot(kind,o); pthread_mutex_unlock(&lock);
  if(!h)free(o); return h; }
void *gh_lookup(uint32_t handle,uint32_t expected,uint32_t *kind_out){
  void *r=NULL; uint32_t i=index_of(handle);
  pthread_mutex_lock(&lock);
  if(i<GH_MAX_HANDLES&&slots[i].kind!=GH_KIND_NONE&&
     (expected==GH_KIND_NONE||slots[i].kind==expected)){
    r=slots[i].object->object; if(kind_out)*kind_out=slots[i].kind; }
  pthread_mutex_unlock(&lock); return r; }
uint32_t gh_duplicate(uint32_t handle){
  uint32_t h=0,i=index_of(handle);
  pthread_mutex_lock(&lock);
  if(i<GH_MAX_HANDLES&&slots[i].kind!=GH_KIND_NONE){
    GhObject *o=slots[i].object; h=take_slot(slots[i].kind,o); if(h)o->refs++; }
  pthread_mutex_unlock(&lock); return h; }
int gh_close(uint32_t handle){
  uint32_t i=index_of(handle); GhObject *dead=NULL;
  pthread_mutex_lock(&lock);
  if(i>=GH_MAX_HANDLES||slots[i].kind==GH_KIND_NONE){pthread_mutex_unlock(&lock);return 0;}
  GhObject *o=slots[i].object; slots[i].kind=GH_KIND_NONE; slots[i].object=NULL;
  if(--o->refs==0)dead=o;
  pthread_mutex_unlock(&lock);
  if(dead){ if(dead->destroy)dead->destroy(dead->object); free(dead); }
  return 1; }
uint32_t gh_live_count(void){
  uint32_t n=0; pthread_mutex_lock(&lock);
  for(uint32_t i=0;i<GH_MAX_HANDLES;i++)if(slots[i].kind!=GH_KIND_NONE)n++;
  pthread_mutex_unlock(&lock); return n; }
void gh_reset_for_tests(void){
  pthread_mutex_lock(&lock);
  for(uint32_t i=0;i<GH_MAX_HANDLES;i++){ if(slots[i].kind!=GH_KIND_NONE){
      GhObject *o=slots[i].object; if(--o->refs==0){ if(o->destroy)o->destroy(o->object); free(o);} }
    slots[i].kind=GH_KIND_NONE; slots[i].object=NULL; }
  next_hint=0; pthread_mutex_unlock(&lock); }
