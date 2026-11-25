/* Development tool: run the static image from an adb shell, without the
 * activity (no window, so nothing can be rendered).
 *
 *   port_cli <libburnout_static.so> <game root> <user root>
 *
 * It loads the image exactly like android/jni/port_main.c does and calls
 * port_entry(). The game then runs its start-up (CRT, configuration, threads,
 * device detection) on the real CPU until it needs a display. That is enough
 * to check a new build of the lifted code on arm64 while the screen is in use
 * by something else; the image's stderr goes to the terminal. */
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char **argv) {
    if (argc < 4) { fprintf(stderr, "usage: %s <image.so> <game root> <user root>\n", argv[0]); return 2; }
    void *image = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!image) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    int (*entry)(const char *, const char *) = (int (*)(const char *, const char *))dlsym(image, "port_entry");
    if (!entry) { fprintf(stderr, "port_entry: %s\n", dlerror()); return 1; }
    int rc = entry(argv[2], argv[3]);
    fprintf(stderr, "port_entry -> %d\n", rc);
    return rc;
}
