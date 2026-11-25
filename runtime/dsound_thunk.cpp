// DSOUND.dll for the 32-bit guest: IDirectSound8 / IDirectSoundBuffer8 on a
// software mixer.
//
// The mixer is the audio device of this platform: a 48 kHz stereo stream that
// is filled from the playing guest buffers in real time. On Android the stream
// is an AAudio output (data callback pulls from the mixer); on the host a
// clock-driven thread plays the same role and can capture the mix to a WAV
// file (PORT_AUDIO_WAV=<path>) so what the game plays can be listened to.
//
// Buffer memory lives in guest memory: Lock returns pointers straight into
// the buffer (two segments on wrap), exactly like DirectSound, so nothing is
// copied on Lock/Unlock and the mixer reads what the game wrote.
//
// Not provided (named stops when a game asks for them): 3D buffers
// (DSBCAPS_CTRL3D), effects (DSBCAPS_CTRLFX), capture, hardware buffers.
#include "d3d9_thunk.h"

#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <atomic>
#include <mutex>
#include <unordered_map>
#include <vector>

extern "C" {
uint32_t d3d9t_guest_alloc(uint32_t size);
void d3d9t_guest_free(uint32_t address);
// Signals a guest event handle (position notifications) from a host thread.
// `scratch_teb` is a private TEB-sized block: the mixer is not a guest thread
// and must not touch any guest thread's LastError. Returns 0 on success.
int port_set_guest_event(uint32_t scratch_teb, uint32_t handle);
// Android: start the AAudio output that pulls dsoundt_mix(). Weak: absent on the host.
int port_audio_start(uint32_t rate, uint32_t channels) __attribute__((weak));
}

namespace {

constexpr uint32_t kTrapBase = DSOUNDT_TRAP_BASE;
constexpr uint32_t kVtableBase = 0x6D1F4000u;
constexpr uint32_t kMagic = 0x444E5344u;            // "DSND"
constexpr uint32_t kRate = 48000, kChannels = 2;
constexpr uint32_t DS_OK = 0, DSERR_INVALIDPARAM = 0x80070057u, DSERR_INVALIDCALL = 0x88780032u,
                   DSERR_BADFORMAT = 0x88780064u, DSERR_UNSUPPORTED = 0x80004001u, E_NOINTERFACE_ = 0x80004002u,
                   DSERR_CONTROLUNAVAIL = 0x8878001Eu, DSERR_NOAGGREGATION = 0x80040110u;
constexpr uint32_t DSBCAPS_PRIMARYBUFFER = 0x1, DSBCAPS_CTRL3D = 0x10, DSBCAPS_CTRLFREQUENCY = 0x20, DSBCAPS_CTRLPAN = 0x40,
                   DSBCAPS_CTRLVOLUME = 0x80, DSBCAPS_CTRLPOSITIONNOTIFY = 0x100, DSBCAPS_CTRLFX = 0x200, DSBCAPS_LOCSOFTWARE = 0x8;
constexpr uint32_t DSBSTATUS_PLAYING = 1, DSBSTATUS_LOOPING = 4;

enum { IF_SOUND = 0, IF_BUFFER = 1, IF_NOTIFY = 2, IF_COUNT_ = 3 };
struct MethodInfo { const char *name; uint8_t words; };
const MethodInfo kSound[] = { {"QueryInterface", 3}, {"AddRef", 1}, {"Release", 1}, {"CreateSoundBuffer", 4}, {"GetCaps", 2},
    {"DuplicateSoundBuffer", 3}, {"SetCooperativeLevel", 3}, {"Compact", 1}, {"GetSpeakerConfig", 2}, {"SetSpeakerConfig", 2},
    {"Initialize", 2}, {"VerifyCertification", 2} };
const MethodInfo kBuffer[] = { {"QueryInterface", 3}, {"AddRef", 1}, {"Release", 1}, {"GetCaps", 2}, {"GetCurrentPosition", 3},
    {"GetFormat", 4}, {"GetVolume", 2}, {"GetPan", 2}, {"GetFrequency", 2}, {"GetStatus", 2}, {"Initialize", 3}, {"Lock", 8},
    {"Play", 4}, {"SetCurrentPosition", 2}, {"SetFormat", 2}, {"SetVolume", 2}, {"SetPan", 2}, {"SetFrequency", 2}, {"Stop", 1},
    {"Unlock", 5}, {"Restore", 1}, {"SetFX", 4}, {"AcquireResources", 4}, {"GetObjectInPath", 5} };
const MethodInfo kNotify[] = { {"QueryInterface", 3}, {"AddRef", 1}, {"Release", 1}, {"SetNotificationPositions", 3} };
const struct { const char *name; const MethodInfo *methods; uint32_t count; } kInterfaces[IF_COUNT_] = {
    { "IDirectSound8", kSound, sizeof kSound / sizeof *kSound },
    { "IDirectSoundBuffer8", kBuffer, sizeof kBuffer / sizeof *kBuffer },
    { "IDirectSoundNotify", kNotify, sizeof kNotify / sizeof *kNotify } };

uint8_t *g_base;
thread_local const char *t_stop = "";
thread_local char t_stop_text[160];
int stop(const char *why) { t_stop = why; return -1; }
inline uint32_t R32(uint32_t va) { uint32_t v; memcpy(&v, g_base + va, 4); return v; }
inline uint16_t R16(uint32_t va) { uint16_t v; memcpy(&v, g_base + va, 2); return v; }
inline void W32(uint32_t va, uint32_t v) { memcpy(g_base + va, &v, 4); }
#define LOGF(...) do { fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } while (0)

struct Format { uint16_t tag = 1, channels = 2; uint32_t rate = 44100, avg = 176400; uint16_t align = 4, bits = 16; bool is_float = false; };
struct Notify { uint32_t offset, event; };
struct Sound {                                              // one buffer (or the device object)
    int iface; uint32_t refs = 1;
    // buffer
    bool primary = false; uint32_t flags = 0, bytes = 0, data = 0;
    Format format;
    bool playing = false, looping = false;
    double position = 0;                                    // play cursor in frames (fractional)
    int32_t volume = 0, pan = 0; uint32_t frequency = 0;    // 0 = original rate
    std::vector<Notify> notifies;
    uint32_t notify_object = 0;                             // guest IDirectSoundNotify of this buffer
    uint32_t owner = 0;                                     // buffer a notify object belongs to
};
std::mutex g_lock;                                          // objects and every buffer field
std::unordered_map<uint32_t, Sound *> g_objects;
Format g_primary_format;
uint32_t g_speaker_config = 4;                              // DSSPEAKER_STEREO
std::atomic<uint64_t> g_frames_mixed{0};

uint32_t make_object(Sound *sound) {
    uint32_t va = d3d9t_guest_alloc(16);
    if (!va) { delete sound; return 0; }
    W32(va, kVtableBase + (uint32_t)sound->iface * 0x200u); W32(va + 4, kMagic); W32(va + 8, (uint32_t)sound->iface); W32(va + 12, 0);
    std::lock_guard<std::mutex> guard(g_lock);
    g_objects[va] = sound;
    return va;
}
Sound *object_of(uint32_t va) {
    std::lock_guard<std::mutex> guard(g_lock);
    auto found = g_objects.find(va);
    return found == g_objects.end() ? nullptr : found->second;
}

// WAVEFORMATEX (18 bytes; cbSize may be absent for PCM) or WAVEFORMATEXTENSIBLE.
bool read_format(uint32_t va, Format *out) {
    Format f;
    f.tag = R16(va); f.channels = R16(va + 2); f.rate = R32(va + 4); f.avg = R32(va + 8); f.align = R16(va + 12); f.bits = R16(va + 14);
    if (f.tag == 0xFFFE) {                                  // extensible: SubFormat at +24
        uint16_t sub = R16(va + 24);
        f.is_float = sub == 3;
        if (sub != 1 && sub != 3) return false;
    } else if (f.tag == 3) f.is_float = true;
    else if (f.tag != 1) return false;
    if (!f.channels || f.channels > 8 || !f.rate || !f.align) return false;
    if (f.is_float ? f.bits != 32 : (f.bits != 8 && f.bits != 16)) return false;
    if (f.align != f.channels * (f.bits / 8)) return false;
    *out = f;
    return true;
}
void format_bytes(const Format &f, uint8_t out[18]) {
    uint16_t tag = f.tag == 0xFFFE ? (uint16_t)(f.is_float ? 3 : 1) : f.tag, zero = 0;
    memcpy(out, &tag, 2); memcpy(out + 2, &f.channels, 2); memcpy(out + 4, &f.rate, 4); memcpy(out + 8, &f.avg, 4);
    memcpy(out + 12, &f.align, 2); memcpy(out + 14, &f.bits, 2); memcpy(out + 16, &zero, 2);
}

// ------------------------------------------------------------------- mixer
inline float gain_of(int32_t hundredths_db) { return hundredths_db <= -10000 ? 0.0f : powf(10.0f, (float)hundredths_db / 2000.0f); }

inline float sample_at(const Sound &s, const uint8_t *data, uint32_t frame, uint32_t channel) {
    const uint8_t *p = data + (size_t)frame * s.format.align + (size_t)channel * (s.format.bits / 8);
    if (s.format.is_float) { float v; memcpy(&v, p, 4); return v; }
    if (s.format.bits == 16) { int16_t v; memcpy(&v, p, 2); return v / 32768.0f; }
    return ((int)*p - 128) / 128.0f;
}

// Mixes `frames` stereo float frames (interleaved) from every playing buffer.
void mix_locked(float *out, uint32_t frames) {
    memset(out, 0, sizeof(float) * frames * kChannels);
    for (auto &entry : g_objects) {
        Sound &s = *entry.second;
        if (s.iface != IF_BUFFER || s.primary || !s.playing || !s.data || !s.bytes) continue;
        uint32_t total = s.bytes / s.format.align;
        if (!total) continue;
        double step = (double)(s.frequency ? s.frequency : s.format.rate) / kRate;
        float volume = gain_of(s.volume);
        float left = volume * (s.pan > 0 ? gain_of(-s.pan) : 1.0f), right = volume * (s.pan < 0 ? gain_of(s.pan) : 1.0f);
        const uint8_t *data = g_base + s.data;
        double before = s.position;
        for (uint32_t i = 0; i < frames; i++) {
            uint32_t frame = (uint32_t)s.position;
            if (frame >= total) {
                if (!s.looping) { s.playing = false; s.position = 0; break; }
                s.position -= total; frame = (uint32_t)s.position;
                if (frame >= total) { s.position = 0; frame = 0; }
            }
            float l = sample_at(s, data, frame, 0), r = s.format.channels > 1 ? sample_at(s, data, frame, 1) : l;
            out[i * 2] += l * left; out[i * 2 + 1] += r * right;
            s.position += step;
        }
        if (!s.notifies.empty()) {                          // position notifications crossed in this block
            uint32_t from = (uint32_t)before * s.format.align, to = (uint32_t)s.position * s.format.align;
            for (const Notify &n : s.notifies) {
                bool hit = n.offset == 0xFFFFFFFFu ? !s.playing
                         : (from <= to ? (n.offset >= from && n.offset < to) : (n.offset >= from || n.offset < to));
                if (hit) {
                    static uint32_t scratch_teb;
                    if (!scratch_teb) scratch_teb = d3d9t_guest_alloc(0x1000);
                    port_set_guest_event(scratch_teb, n.event);
                }
            }
        }
    }
    g_frames_mixed.fetch_add(frames);
}

// Host: a clock keeps the cursors moving at real-time rate.
FILE *g_wav; uint32_t g_wav_bytes;
void *mixer_thread(void *) {
    pthread_setname_np(pthread_self(), "port-mixer");
    const uint32_t period = kRate / 100;                    // 10 ms
    std::vector<float> block(period * kChannels);
    std::vector<int16_t> pcm(period * kChannels);
    timespec next; clock_gettime(CLOCK_MONOTONIC, &next);
    for (;;) {
        { std::lock_guard<std::mutex> guard(g_lock); mix_locked(block.data(), period); }
        if (g_wav) {
            for (size_t i = 0; i < pcm.size(); i++) { float v = block[i]; v = v > 1 ? 1 : (v < -1 ? -1 : v); pcm[i] = (int16_t)lrintf(v * 32767.0f); }
            fwrite(pcm.data(), 2, pcm.size(), g_wav); g_wav_bytes += (uint32_t)pcm.size() * 2;
            if ((g_frames_mixed.load() % (kRate * 2)) < period) {   // refresh the header every 2 s
                long at = ftell(g_wav); uint32_t riff = 36 + g_wav_bytes;
                fseek(g_wav, 4, SEEK_SET); fwrite(&riff, 4, 1, g_wav); fseek(g_wav, 40, SEEK_SET); fwrite(&g_wav_bytes, 4, 1, g_wav);
                fseek(g_wav, at, SEEK_SET); fflush(g_wav);
            }
        }
        next.tv_nsec += 10000000; if (next.tv_nsec >= 1000000000) { next.tv_nsec -= 1000000000; next.tv_sec++; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, nullptr);
    }
    return nullptr;
}
std::once_flag g_device_once;
void start_device() {
    if (port_audio_start && !port_audio_start(kRate, kChannels)) { LOGF("DSOUND output: platform audio stream %u Hz stereo", (unsigned)kRate); return; }
    const char *wav = getenv("PORT_AUDIO_WAV");
    if (wav && (g_wav = fopen(wav, "wb"))) {
        uint8_t header[44] = { 'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0 };
        uint32_t rate = kRate, avg = kRate * 4; uint16_t align = 4, bits = 16;
        memcpy(header + 24, &rate, 4); memcpy(header + 28, &avg, 4); memcpy(header + 32, &align, 2); memcpy(header + 34, &bits, 2);
        memcpy(header + 36, "data", 4);
        fwrite(header, 1, sizeof header, g_wav);
    }
    pthread_t thread;
    pthread_create(&thread, nullptr, mixer_thread, nullptr);
    pthread_detach(thread);
    LOGF("DSOUND output: clock-driven mixer %u Hz stereo%s%s", (unsigned)kRate, g_wav ? ", capture " : " (no device on this host)", g_wav ? wav : "");
}

// ----------------------------------------------------------- IDirectSound8
int create_buffer(const uint32_t *a, uint32_t *r) {         // (this, pcDesc, ppBuffer, pUnkOuter)
    if (!a[1] || !a[2]) { *r = DSERR_INVALIDPARAM; return 0; }
    if (a[3]) { *r = DSERR_NOAGGREGATION; return 0; }
    uint32_t size = R32(a[1]), flags = R32(a[1] + 4), bytes = R32(a[1] + 8), wfx = R32(a[1] + 16);
    if (size != 36 && size != 20) { *r = DSERR_INVALIDPARAM; return 0; }
    if (flags & DSBCAPS_CTRL3D) return stop("CreateSoundBuffer: DSBCAPS_CTRL3D");
    if (flags & DSBCAPS_CTRLFX) return stop("CreateSoundBuffer: DSBCAPS_CTRLFX");
    Sound *sound = new Sound();
    sound->iface = IF_BUFFER; sound->flags = flags;
    if (flags & DSBCAPS_PRIMARYBUFFER) {
        if (bytes || wfx) { delete sound; *r = DSERR_INVALIDPARAM; return 0; }
        sound->primary = true;
        sound->format = g_primary_format;
    } else {
        if (!wfx || !read_format(wfx, &sound->format)) { delete sound; *r = wfx ? DSERR_BADFORMAT : DSERR_INVALIDPARAM; return 0; }
        if (bytes < 4 || bytes > 0x0FFFFFFFu) { delete sound; *r = DSERR_INVALIDPARAM; return 0; }
        sound->bytes = bytes;
        sound->data = d3d9t_guest_alloc(bytes);
        if (!sound->data) { delete sound; *r = 0x8007000Eu; return 0; }
        memset(g_base + sound->data, sound->format.bits == 8 ? 0x80 : 0, bytes);
    }
    static int trace = -1;
    if (trace < 0) trace = getenv("PORT_TRACE_AUDIO") ? 1 : 0;
    if (trace || sound->primary) LOGF("DSOUND CreateSoundBuffer flags=0x%X bytes=%u fmt=%u ch=%u rate=%u bits=%u", (unsigned)flags, (unsigned)bytes,
                                      (unsigned)sound->format.tag, (unsigned)sound->format.channels, (unsigned)sound->format.rate, (unsigned)sound->format.bits);
    uint32_t object = make_object(sound);
    W32(a[2], object);
    *r = object ? DS_OK : 0x8007000Eu;
    std::call_once(g_device_once, start_device);
    return 0;
}

int sound_method(Sound *self, uint32_t self_va, uint32_t slot, const uint32_t *a, uint32_t *r) {
    switch (slot) {
    case 3: return create_buffer(a, r);
    case 4: {                                               // GetCaps(DSCAPS)
        if (!a[1] || R32(a[1]) < 96) { *r = DSERR_INVALIDPARAM; return 0; }
        uint32_t caps[24]; memset(caps, 0, sizeof caps);
        caps[0] = 96;
        // primary mono/stereo/8/16 bit, continuous rate, secondary mono/stereo/8/16 bit; no hardware buffers.
        caps[1] = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x100 | 0x200 | 0x400 | 0x800;
        caps[2] = 100; caps[3] = 200000;                    // DSBFREQUENCY_MIN / MAX
        caps[4] = 1;                                        // one primary buffer
        memcpy(g_base + a[1], caps, 96);
        *r = DS_OK;
        return 0;
    }
    case 5: return stop("DuplicateSoundBuffer");
    case 6: *r = DS_OK; return 0;                           // SetCooperativeLevel: one application owns the device
    case 7: *r = DS_OK; return 0;                           // Compact
    case 8: if (!a[1]) { *r = DSERR_INVALIDPARAM; return 0; } W32(a[1], g_speaker_config); *r = DS_OK; return 0;
    case 9: g_speaker_config = a[1]; *r = DS_OK; return 0;
    case 10: *r = 0x8878000Au; return 0;                    // Initialize: DSERR_ALREADYINITIALIZED
    case 11: if (a[1]) W32(a[1], 0); *r = DS_OK; return 0;  // VerifyCertification: DS_UNCERTIFIED
    default: return stop("bad slot");
    }
    (void)self; (void)self_va;
}

// ----------------------------------------------------- IDirectSoundBuffer8
int buffer_method(Sound *self, uint32_t self_va, uint32_t slot, const uint32_t *a, uint32_t *r) {
    std::unique_lock<std::mutex> guard(g_lock);
    uint32_t align = self->format.align ? self->format.align : 1;
    switch (slot) {
    case 3:                                                 // GetCaps(DSBCAPS 20 bytes)
        if (!a[1] || R32(a[1]) < 20) { *r = DSERR_INVALIDPARAM; return 0; }
        W32(a[1] + 4, self->flags | DSBCAPS_LOCSOFTWARE); W32(a[1] + 8, self->bytes); W32(a[1] + 12, 0); W32(a[1] + 16, 0);
        *r = DS_OK; return 0;
    case 4: {                                               // GetCurrentPosition(pPlay, pWrite)
        if (self->primary) { *r = DSERR_INVALIDCALL; return 0; }
        uint32_t play = ((uint32_t)self->position * align) % (self->bytes ? self->bytes : 1);
        // The write cursor leads by the mixer period (10 ms) plus safety, as on a software device.
        uint32_t lead = (uint32_t)((uint64_t)(self->frequency ? self->frequency : self->format.rate) * 15 / 1000) * align;
        if (a[1]) W32(a[1], play);
        if (a[2]) W32(a[2], self->playing ? (play + lead) % self->bytes : play);
        *r = DS_OK; return 0;
    }
    case 5: {                                               // GetFormat(pwfx, dwSizeAllocated, pdwSizeWritten)
        const Format &f = self->primary ? g_primary_format : self->format;
        if (a[1]) {
            if (a[2] < 16) { *r = DSERR_INVALIDPARAM; return 0; }
            uint8_t bytes[18]; format_bytes(f, bytes);
            uint32_t n = a[2] >= 18 ? 18 : 16;              // PCMWAVEFORMAT or WAVEFORMATEX
            memcpy(g_base + a[1], bytes, n);
            if (a[3]) W32(a[3], n);
        } else if (a[3]) W32(a[3], 18);
        *r = DS_OK; return 0;
    }
    case 6: if (!a[1]) { *r = DSERR_INVALIDPARAM; return 0; } W32(a[1], (uint32_t)self->volume); *r = DS_OK; return 0;
    case 7: if (!a[1]) { *r = DSERR_INVALIDPARAM; return 0; } W32(a[1], (uint32_t)self->pan); *r = DS_OK; return 0;
    case 8: if (!a[1]) { *r = DSERR_INVALIDPARAM; return 0; } W32(a[1], self->frequency ? self->frequency : self->format.rate); *r = DS_OK; return 0;
    case 9:                                                 // GetStatus
        if (!a[1]) { *r = DSERR_INVALIDPARAM; return 0; }
        W32(a[1], (self->playing ? DSBSTATUS_PLAYING : 0) | (self->playing && self->looping ? DSBSTATUS_LOOPING : 0));
        *r = DS_OK; return 0;
    case 10: *r = 0x8878000Au; return 0;                    // Initialize: already initialized
    case 11: {                                              // Lock(offset, bytes, ppv1, pcb1, ppv2, pcb2, flags)
        if (self->primary) { *r = DSERR_INVALIDCALL; return 0; }
        uint32_t offset = a[1], bytes = a[2], flags = a[7];
        if (flags & 1) {                                    // DSBLOCK_FROMWRITECURSOR
            uint32_t lead = (uint32_t)((uint64_t)(self->frequency ? self->frequency : self->format.rate) * 15 / 1000) * align;
            offset = (((uint32_t)self->position * align) + (self->playing ? lead : 0)) % self->bytes;
        }
        if (flags & 2) bytes = self->bytes;                 // DSBLOCK_ENTIREBUFFER
        if (!a[3] || !a[4] || offset >= self->bytes || !bytes || bytes > self->bytes) { *r = DSERR_INVALIDPARAM; return 0; }
        uint32_t first = bytes < self->bytes - offset ? bytes : self->bytes - offset;
        W32(a[3], self->data + offset); W32(a[4], first);
        if (a[5]) W32(a[5], bytes > first ? self->data : 0);
        if (a[6]) W32(a[6], bytes - first);
        *r = DS_OK; return 0;
    }
    case 12:                                                // Play(reserved, priority, flags)
        if (self->primary) { *r = DS_OK; return 0; }
        self->looping = (a[3] & 1) != 0; self->playing = true;
        *r = DS_OK; return 0;
    case 13:                                                // SetCurrentPosition(bytes)
        if (self->primary || a[1] >= self->bytes) { *r = DSERR_INVALIDPARAM; return 0; }
        self->position = a[1] / align;
        *r = DS_OK; return 0;
    case 14: {                                              // SetFormat: primary only
        Format f;
        if (!self->primary) { *r = DSERR_INVALIDCALL; return 0; }
        if (!a[1] || !read_format(a[1], &f)) { *r = DSERR_BADFORMAT; return 0; }
        g_primary_format = f; self->format = f;
        LOGF("DSOUND primary format: %u ch %u Hz %u bit", (unsigned)f.channels, (unsigned)f.rate, (unsigned)f.bits);
        *r = DS_OK; return 0;
    }
    case 15:                                                // SetVolume
        if (!(self->flags & DSBCAPS_CTRLVOLUME) && !self->primary) { *r = DSERR_CONTROLUNAVAIL; return 0; }
        if ((int32_t)a[1] > 0 || (int32_t)a[1] < -10000) { *r = DSERR_INVALIDPARAM; return 0; }
        self->volume = (int32_t)a[1]; *r = DS_OK; return 0;
    case 16:                                                // SetPan
        if (!(self->flags & DSBCAPS_CTRLPAN)) { *r = DSERR_CONTROLUNAVAIL; return 0; }
        if ((int32_t)a[1] > 10000 || (int32_t)a[1] < -10000) { *r = DSERR_INVALIDPARAM; return 0; }
        self->pan = (int32_t)a[1]; *r = DS_OK; return 0;
    case 17:                                                // SetFrequency
        if (!(self->flags & DSBCAPS_CTRLFREQUENCY)) { *r = DSERR_CONTROLUNAVAIL; return 0; }
        if (a[1] && (a[1] < 100 || a[1] > 200000)) { *r = DSERR_INVALIDPARAM; return 0; }
        self->frequency = a[1]; *r = DS_OK; return 0;
    case 18: self->playing = false; *r = DS_OK; return 0;   // Stop keeps the position
    case 19: *r = DS_OK; return 0;                          // Unlock: the memory is the buffer
    case 20: *r = DS_OK; return 0;                          // Restore: buffers are never lost
    case 21: return stop("SetFX");
    case 22: return stop("AcquireResources");
    case 23: return stop("GetObjectInPath");
    default: return stop("bad slot");
    }
    (void)self_va;
}

int notify_method(Sound *self, uint32_t slot, const uint32_t *a, uint32_t *r) {
    if (slot != 3) return stop("bad slot");                 // SetNotificationPositions(count, DSBPOSITIONNOTIFY*)
    std::lock_guard<std::mutex> guard(g_lock);
    auto owner = g_objects.find(self->owner);
    if (owner == g_objects.end()) { *r = DSERR_INVALIDCALL; return 0; }
    if (owner->second->playing) { *r = DSERR_INVALIDCALL; return 0; }
    if (a[1] && !a[2]) { *r = DSERR_INVALIDPARAM; return 0; }
    owner->second->notifies.clear();
    for (uint32_t i = 0; i < a[1] && i < 1024; i++) owner->second->notifies.push_back(Notify{ R32(a[2] + i * 8), R32(a[2] + i * 8 + 4) });
    *r = DS_OK;
    return 0;
}

int call_method(Sound *self, uint32_t self_va, uint32_t slot, const uint32_t *a, uint32_t *r) {
    static const uint8_t iid_unknown[16] = { 0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46 };
    // IDirectSound {279AFA83-4981-11CE-A521-0020AF0BE560}, IDirectSound8 {C50A7E93-F395-4834-9EF6-7FA99DE50966}
    static const uint8_t iid_ds[16] = { 0x83, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 };
    static const uint8_t iid_ds8[16] = { 0x93, 0x7E, 0x0A, 0xC5, 0x95, 0xF3, 0x34, 0x48, 0x9E, 0xF6, 0x7F, 0xA9, 0x9D, 0xE5, 0x09, 0x66 };
    // IDirectSoundBuffer {279AFA85-4981-11CE-A521-0020AF0BE560}, IDirectSoundBuffer8 {6825A449-7524-4D82-920F-50E36AB3AB1E}
    static const uint8_t iid_buf[16] = { 0x85, 0xFA, 0x9A, 0x27, 0x81, 0x49, 0xCE, 0x11, 0xA5, 0x21, 0x00, 0x20, 0xAF, 0x0B, 0xE5, 0x60 };
    static const uint8_t iid_buf8[16] = { 0x49, 0xA4, 0x25, 0x68, 0x24, 0x75, 0x82, 0x4D, 0x92, 0x0F, 0x50, 0xE3, 0x6A, 0xB3, 0xAB, 0x1E };
    // IDirectSoundNotify {B0210783-89CD-11D0-AF08-00A0C925CD16}
    static const uint8_t iid_notify[16] = { 0x83, 0x07, 0x21, 0xB0, 0xCD, 0x89, 0xD0, 0x11, 0xAF, 0x08, 0x00, 0xA0, 0xC9, 0x25, 0xCD, 0x16 };
    switch (slot) {
    case 0: {
        if (!a[1] || !a[2]) { *r = 0x80004003u; return 0; }
        const uint8_t *iid = g_base + a[1];
        bool same = !memcmp(iid, iid_unknown, 16) ||
                    (self->iface == IF_SOUND && (!memcmp(iid, iid_ds, 16) || !memcmp(iid, iid_ds8, 16))) ||
                    (self->iface == IF_BUFFER && (!memcmp(iid, iid_buf, 16) || (!self->primary && !memcmp(iid, iid_buf8, 16)))) ||
                    (self->iface == IF_NOTIFY && !memcmp(iid, iid_notify, 16));
        if (same) { std::lock_guard<std::mutex> guard(g_lock); self->refs++; W32(a[2], self_va); *r = DS_OK; return 0; }
        if (self->iface == IF_BUFFER && !self->primary && !memcmp(iid, iid_notify, 16)) {
            if (!(self->flags & DSBCAPS_CTRLPOSITIONNOTIFY)) { W32(a[2], 0); *r = E_NOINTERFACE_; return 0; }
            uint32_t object;
            { std::lock_guard<std::mutex> guard(g_lock); object = self->notify_object; }
            if (!object) {
                Sound *notify = new Sound(); notify->iface = IF_NOTIFY; notify->owner = self_va;
                object = make_object(notify);
                std::lock_guard<std::mutex> guard(g_lock); self->notify_object = object;
            } else { std::lock_guard<std::mutex> guard(g_lock); g_objects[object]->refs++; }
            W32(a[2], object); *r = DS_OK; return 0;
        }
        static int logged;
        if (logged++ < 8) LOGF("DSOUND QueryInterface: %s has no interface %08X-%04X-...", kInterfaces[self->iface].name, (unsigned)R32(a[1]), (unsigned)R16(a[1] + 4));
        W32(a[2], 0); *r = E_NOINTERFACE_;
        return 0;
    }
    case 1: { std::lock_guard<std::mutex> guard(g_lock); *r = ++self->refs; return 0; }
    case 2: {
        uint32_t data = 0;
        {
            std::lock_guard<std::mutex> guard(g_lock);
            uint32_t left = --self->refs;
            *r = left;
            if (left) return 0;
            g_objects.erase(self_va);
            data = self->data;
            if (self->iface == IF_NOTIFY) { auto owner = g_objects.find(self->owner); if (owner != g_objects.end()) owner->second->notify_object = 0; }
        }
        d3d9t_guest_free(data); d3d9t_guest_free(self_va);
        delete self;
        return 0;
    }
    default: break;
    }
    if (self->iface == IF_SOUND) return sound_method(self, self_va, slot, a, r);
    if (self->iface == IF_BUFFER) return buffer_method(self, self_va, slot, a, r);
    return notify_method(self, slot, a, r);
}

}  // namespace

extern "C" void dsoundt_init(uint8_t *flat) {
    g_base = flat;
    for (uint32_t iface = 0; iface < IF_COUNT_; iface++)
        for (uint32_t slot = 0; slot < kInterfaces[iface].count; slot++)
            W32(kVtableBase + iface * 0x200u + slot * 4u, kTrapBase + (iface << 12) + slot * 8u);
    g_primary_format = Format();
    g_primary_format.rate = 22050; g_primary_format.bits = 8; g_primary_format.align = 2; g_primary_format.avg = 44100;   // DirectSound's default primary format
}

// The platform audio stream pulls from here (AAudio data callback): interleaved
// stereo float frames. Never blocks for long: the lock only guards the mix.
extern "C" void dsoundt_mix(float *out, uint32_t frames) {
    std::lock_guard<std::mutex> guard(g_lock);
    mix_locked(out, frames);
}

// DSOUND.dll!GetDeviceID(pGuidSrc, pGuidDest): the default devices map onto the
// one output device of this platform.
extern "C" int dsoundt_get_device_id(const uint32_t *a, uint32_t *r) {
    // DSDEVID_DefaultPlayback {DEF00000-9C6D-47ED-AAF1-4DDA8F2B5C03}, ...VoicePlayback {DEF00002-...}
    static const uint8_t tail[12] = { 0x6D, 0x9C, 0xED, 0x47, 0xAA, 0xF1, 0x4D, 0xDA, 0x8F, 0x2B, 0x5C, 0x03 };
    // The platform's output device (the software mixer of this runtime).
    static const uint8_t device[16] = { 'P', 'O', 'R', 'T', 'D', 'S', 'N', 'D', 0x80, 0x00, 'M', 'I', 'X', 'E', 'R', 0x01 };
    if (!a[0] || !a[1]) { *r = DSERR_INVALIDPARAM; return 0; }
    const uint8_t *source = g_base + a[0];
    uint32_t kind; memcpy(&kind, source, 4);
    if (!memcmp(source + 4, tail, 12) && (kind == 0xDEF00000u || kind == 0xDEF00002u)) { memcpy(g_base + a[1], device, 16); *r = DS_OK; return 0; }
    if (!memcmp(source + 4, tail, 12) && (kind == 0xDEF00001u || kind == 0xDEF00003u)) { *r = 0x88780078u; return 0; }   // capture: DSERR_NODRIVER
    if (!memcmp(source, device, 16)) { memcpy(g_base + a[1], device, 16); *r = DS_OK; return 0; }
    *r = DSERR_INVALIDPARAM;
    return 0;
}

// DirectSoundCreate / DirectSoundCreate8 (pcGuidDevice, ppDS, pUnkOuter)
extern "C" int dsoundt_create(const uint32_t *a, uint32_t *r) {
    if (!a[1]) { *r = DSERR_INVALIDPARAM; return 0; }
    if (a[2]) { W32(a[1], 0); *r = DSERR_NOAGGREGATION; return 0; }
    Sound *sound = new Sound();
    sound->iface = IF_SOUND;
    uint32_t object = make_object(sound);
    W32(a[1], object);
    *r = object ? DS_OK : 0x8007000Eu;
    LOGF("DSOUND DirectSoundCreate -> 0x%08X", (unsigned)object);
    return 0;
}

extern "C" const char *dsoundt_trap_name(uint32_t trap) {
    static thread_local char text[96];
    uint32_t iface = (trap - kTrapBase) >> 12, slot = ((trap - kTrapBase) & 0xFFFu) / 8u;
    if (iface >= IF_COUNT_ || slot >= kInterfaces[iface].count) return "DSOUND::?";
    snprintf(text, sizeof text, "%s::%s", kInterfaces[iface].name, kInterfaces[iface].methods[slot].name);
    return text;
}
extern "C" const char *dsoundt_last_stop(void) { return t_stop; }

extern "C" int dsoundt_call(uint32_t trap, const uint32_t *args, uint32_t *eax) {
    uint32_t iface = (trap - kTrapBase) >> 12, slot = ((trap - kTrapBase) & 0xFFFu) / 8u;
    t_stop = "";
    if (iface >= IF_COUNT_ || slot >= kInterfaces[iface].count || ((trap - kTrapBase) & 7u)) { t_stop = "bad-trap-address"; return -1; }
    Sound *self = object_of(args[0]);
    if (!self || self->iface != (int)iface) { t_stop = "unknown-this"; return -1; }
    static int trace = -1;
    if (trace < 0) trace = getenv("PORT_TRACE_AUDIO") ? 1 : 0;
    int rc = call_method(self, args[0], slot, args, eax);
    if (trace) fprintf(stderr, "DSOUND %s(%08X, %08X, %08X, %08X) -> 0x%08X%s\n", dsoundt_trap_name(trap), args[0], args[1], args[2], args[3], *eax, rc ? " STOP" : "");
    if (rc) {
        snprintf(t_stop_text, sizeof t_stop_text, "%s: %s", dsoundt_trap_name(trap), t_stop);
        t_stop = t_stop_text;
        return -1;
    }
    return (int)(kInterfaces[iface].methods[slot].words * 4u);
}
