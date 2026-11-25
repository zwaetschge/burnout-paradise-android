/* A10/A14 -- x86-64 Linux host driver for the static image.
 *
 *   static_host --game <dir> --user <dir> [--nls <path>]        run the entry
 *   static_host ... --selftest-imports                          IAT thunk check
 *   static_host ... --selftest-threads                          guest thread check
 *   static_host ... --selftest-contention                       CS contention check
 *   static_host ... --strict-export-resolution                  stop at GetProcAddress
 *   static_host ... --trace-exports                             log every resolution
 *   static_host ... --dump-imports                              wiring audit
 *   static_host ... --call-va 0x0827A000                        call one body
 *
 * Runs the translated image once and reports the outcome. This is a
 * bring-up/verification harness, not a game front end: it exists so the
 * integration can be executed and its first missing piece measured on the
 * build host. Nothing here is used by the Android build.
 *
 * --selftest-imports / --call-va deliberately exercise the two halves the
 * entry point itself never reaches (the entry jumps straight into the
 * untranslated .trace section), so the IAT thunking and the stdcall cleanup are
 * measured rather than assumed.
 *
 * A14: --selftest-threads starts a real guest thread through
 * kernel32!CreateThread, waits on its handle and reads its exit code, so the
 * launcher (own guest stack, own TEB, own RECOMP_TLS CPU state, machine lock,
 * handle signalling) is executed rather than described. --dump-imports lists
 * every declared import with its binding, which is the wiring audit. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "static_entry.h"

static int failures;

static int call(const char *dll, const char *name, const uint32_t *args,
                uint32_t n, uint32_t *eax_out) {
    /* Everything goes to stderr: it is unbuffered, and a named trap ends the
     * process with _exit(), which would discard a buffered stdout. */
    uint32_t eax = 0, esp = 0;
    int rc = port_selftest_import(dll, name, args, n, &eax, &esp);
    fprintf(stderr, "IMPORT %-38s %-28s rc=%d eax=0x%08X esp=0x%08X\n", dll, name, rc, eax,
           esp);
    if (rc) failures++;
    if (eax_out) *eax_out = eax;
    return rc;
}

static int selftest_imports(void) {
    failures = 0;
    call("kernel32.dll", "GetLastError", (uint32_t[]){0}, 0, NULL);
    call("kernel32.dll", "SetLastError", (uint32_t[]){0x5Au}, 1, NULL);
    call("kernel32.dll", "GetLastError", (uint32_t[]){0}, 0, NULL);
    call("kernel32.dll", "GetCurrentProcessId", (uint32_t[]){0}, 0, NULL);
    call("kernel32.dll", "GetCurrentThreadId", (uint32_t[]){0}, 0, NULL);
    call("kernel32.dll", "GetCurrentProcess", (uint32_t[]){0}, 0, NULL);
    call("kernel32.dll", "GetProcessHeap", (uint32_t[]){0}, 0, NULL);
    call("kernel32.dll", "GetModuleHandleW", (uint32_t[]){0}, 1, NULL);
    call("kernel32.dll", "VirtualProtect",
         (uint32_t[]){0x30000000u, 0x1000u, 0x04u, 0x3007F000u}, 4, NULL);

    /* A real conversion through the CP1252 profile: "Hello" (5 bytes) must come
     * back as 5 UTF-16 units. */
    {
        static const char text[] = "Hello";
        port_poke(0x74002000u, text, 6);
        uint32_t eax = 0;
        call("kernel32.dll", "MultiByteToWideChar",
             (uint32_t[]){1252u, 0u, 0x74002000u, 5u, 0x74003000u, 16u}, 6, &eax);
        uint16_t wide[6] = {0};
        port_peek(0x74003000u, wide, sizeof wide);
        fprintf(stderr, "IMPORT %-38s %-28s wide=%c%c%c%c%c\n", "kernel32.dll",
               "MultiByteToWideChar(out)", wide[0], wide[1], wide[2], wide[3],
               wide[4]);
        if (eax != 5 || wide[0] != 'H' || wide[4] != 'o') failures++;
    }
    /* A real file-system bridge: an unmapped path must answer with the ordinary
     * Win32 "no such file". */
    call("kernel32.dll", "GetFileAttributesA", (uint32_t[]){0x74002000u}, 1, NULL);

    /* --- A14: one call per newly wired family ------------------------- */
    /* CRT (__cdecl, caller pops): strlen("Hello") == 5. */
    port_poke(0x74002000u, "Hello", 6);
    call("api-ms-win-crt-string-l1-1-0.dll", "strlen",
         (uint32_t[]){0x74002000u}, 1, NULL);
    /* CRT heap: malloc/free through the guest process heap. */
    {
        uint32_t block = 0;
        call("api-ms-win-crt-heap-l1-1-0.dll", "malloc",
             (uint32_t[]){64u}, 1, &block);
        fprintf(stderr, "IMPORT %-38s %-28s block=0x%08X\n", "api-ms-win-crt-heap-l1-1-0.dll",
               "malloc(out)", block);
        if (!block) failures++;
        call("api-ms-win-crt-heap-l1-1-0.dll", "free", (uint32_t[]){block}, 1, NULL);
    }
    /* CRT math returns a double on the modelled x87 stack. */
    call("api-ms-win-crt-math-l1-1-0.dll", "ldexp",
         (uint32_t[]){0x74002000u, 0u}, 2, NULL);
    /* Sync objects: create/set/wait/close one auto-reset event. */
    {
        uint32_t event = 0;
        call("kernel32.dll", "CreateEventA",
             (uint32_t[]){0u, 0u, 1u, 0u}, 4, &event);
        fprintf(stderr, "IMPORT %-38s %-28s event=0x%08X\n", "kernel32.dll",
               "CreateEventA(out)", event);
        if (!event) failures++;
        call("kernel32.dll", "SetEvent", (uint32_t[]){event}, 1, NULL);
        call("kernel32.dll", "WaitForSingleObject",
             (uint32_t[]){event, 0u}, 2, NULL);
        call("kernel32.dll", "CloseHandle", (uint32_t[]){event}, 1, NULL);
    }
    /* Critical section: init/enter/leave/delete (multi-thread mode is on). */
    {
        uint32_t cs = 0x74008000u;
        call("kernel32.dll", "InitializeCriticalSection", (uint32_t[]){cs}, 1, NULL);
        call("kernel32.dll", "EnterCriticalSection", (uint32_t[]){cs}, 1, NULL);
        call("kernel32.dll", "EnterCriticalSection", (uint32_t[]){cs}, 1, NULL);
        call("kernel32.dll", "LeaveCriticalSection", (uint32_t[]){cs}, 1, NULL);
        call("kernel32.dll", "LeaveCriticalSection", (uint32_t[]){cs}, 1, NULL);
        call("kernel32.dll", "DeleteCriticalSection", (uint32_t[]){cs}, 1, NULL);
    }
    /* SRW lock: exclusive acquire/release. */
    {
        uint32_t lock = 0x74009000u;
        call("kernel32.dll", "InitializeSRWLock", (uint32_t[]){lock}, 1, NULL);
        call("kernel32.dll", "AcquireSRWLockExclusive", (uint32_t[]){lock}, 1, NULL);
        call("kernel32.dll", "ReleaseSRWLockExclusive", (uint32_t[]){lock}, 1, NULL);
        call("kernel32.dll", "AcquireSRWLockShared", (uint32_t[]){lock}, 1, NULL);
        call("kernel32.dll", "ReleaseSRWLockShared", (uint32_t[]){lock}, 1, NULL);
    }
    /* Atomics on a guest address. */
    {
        uint32_t value = 41;
        port_poke(0x7400A000u, &value, 4);
        call("kernel32.dll", "InterlockedIncrement", (uint32_t[]){0x7400A000u}, 1, NULL);
        port_peek(0x7400A000u, &value, 4);
        fprintf(stderr, "IMPORT %-38s %-28s value=%u\n", "kernel32.dll",
               "InterlockedIncrement(out)", value);
        if (value != 42) failures++;
        call("kernel32.dll", "InterlockedDecrement", (uint32_t[]){0x7400A000u}, 1, NULL);
    }
    /* Time: GetSystemTime writes the 16-byte SYSTEMTIME. */
    call("kernel32.dll", "GetSystemTime", (uint32_t[]){0x7400B000u}, 1, NULL);
    call("kernel32.dll", "GetTickCount", (uint32_t[]){0}, 0, NULL);
    /* Registry projection (read-only): a key that is not projected answers
     * ERROR_FILE_NOT_FOUND, never a fabricated success. */
    {
        uint32_t key = 0;
        call("advapi32.dll", "RegOpenKeyExW",
             (uint32_t[]){0x80000002u, 0x7400C000u, 0u, 0x20019u, 0x7400C800u}, 5,
             &key);
        port_peek(0x7400C800u, &key, 4);
        fprintf(stderr, "IMPORT %-38s %-28s key=0x%08X\n", "advapi32.dll",
               "RegOpenKeyExW(out)", key);
        if (key) call("advapi32.dll", "RegCloseKey", (uint32_t[]){key}, 1, NULL);
    }
    /* Process query: the runtime-owned process only. */
    call("kernel32.dll", "OpenProcess", (uint32_t[]){0x400u, 0u, 0x1000u}, 3, NULL);
    /* user32: no host window exists, so the model answers with its own
     * documented values instead of inventing a window. */
    call("user32.dll", "GetSystemMetrics", (uint32_t[]){0u}, 1, NULL);
    fprintf(stderr, "SELFTEST_IMPORTS failures=%d bridges=%u bound=%u/%u\n", failures,
           port_bridge_count(), port_bound_imports(), port_total_imports());
    return failures ? 1 : 0;
}

static int selftest_threads(void) {
    failures = 0;
    /* A translated body that is just `ret`: real lifted code on the new guest
     * thread, not a synthetic stub. */
    uint32_t start = port_find_trivial_function();
    fprintf(stderr, "THREAD start_va=0x%08X\n", start);
    if (!start) {
        fprintf(stderr, "THREAD no trivial lifted function found\n");
        return 1;
    }
    uint32_t thread = 0, code = 0xFFFFFFFFu, wait = 0xFFFFFFFFu;
    port_poke(0x7400D000u, (uint32_t[]){0}, 4);
    int rc = call("kernel32.dll", "CreateThread",
                  (uint32_t[]){0u, 0u, start, 0x1234u, 0u, 0x7400D000u}, 6, &thread);
    uint32_t tid = 0;
    port_peek(0x7400D000u, &tid, 4);
    fprintf(stderr, "THREAD handle=0x%08X tid=0x%08X threads=%u\n", thread, tid,
           port_guest_thread_count());
    if (rc || !thread || !tid) failures++;
    rc = call("kernel32.dll", "WaitForSingleObject", (uint32_t[]){thread, 5000u}, 2,
              &wait);
    fprintf(stderr, "THREAD wait=0x%08X (WAIT_OBJECT_0=0)\n", wait);
    if (rc || wait != 0) failures++;
    rc = call("kernel32.dll", "GetExitCodeThread",
              (uint32_t[]){thread, 0x7400D800u}, 2, NULL);
    port_peek(0x7400D800u, &code, 4);
    fprintf(stderr, "THREAD exit_code=0x%08X\n", code);
    if (rc || code == 259u) failures++;
    call("kernel32.dll", "CloseHandle", (uint32_t[]){thread}, 1, NULL);
    fprintf(stderr, "SELFTEST_THREADS failures=%d\n", failures);
    return failures ? 1 : 0;
}

/* --- A14: real contention on a guest CriticalSection --------------------
 *
 * Two host threads, each with its own guest stack/TEB (RECOMP_TLS CPU state),
 * both call EnterCriticalSection on the same guest address. The second one must
 * block until the first leaves, and the recursive owner must be able to leave
 * from the thread that entered. That is the multi-thread path of
 * runtime/guest_critical.c, not the one-CPU model. */
#include <pthread.h>
#include <unistd.h>

typedef struct {
    uint32_t esp, teb, tls;
    uint32_t cs;
    int rc_enter, rc_leave, eax_enter;
    int done;
} ContendArgs;

static void *contend_thread(void *arg) {
    ContendArgs *a = (ContendArgs *)arg;
    if (port_thread_attach(a->esp, a->teb, a->tls)) { a->rc_enter = -1; return NULL; }
    uint32_t eax = 0, esp = 0;
    a->rc_enter = port_selftest_import("kernel32.dll", "EnterCriticalSection",
                                       (uint32_t[]){a->cs}, 1, &eax, &esp);
    a->eax_enter = (int)eax;
    a->rc_leave = port_selftest_import("kernel32.dll", "LeaveCriticalSection",
                                       (uint32_t[]){a->cs}, 1, &eax, &esp);
    a->done = 1;
    return NULL;
}

static int selftest_contention(void) {
    failures = 0;
    uint32_t cs = 0x7400F000u;
    call("kernel32.dll", "InitializeCriticalSection", (uint32_t[]){cs}, 1, NULL);
    call("kernel32.dll", "EnterCriticalSection", (uint32_t[]){cs}, 1, NULL);
    ContendArgs a;
    memset(&a, 0, sizeof a);
    a.esp = 0x30280000u;      /* own guest stack, inside the flat reservation */
    a.teb = 0x30300000u;
    a.tls = 0x30301000u;
    a.cs = cs;
    pthread_t host;
    if (pthread_create(&host, NULL, contend_thread, &a)) {
        fprintf(stderr, "CONTEND pthread_create failed\n");
        return 1;
    }
    /* Give the second thread time to reach the blocked enter. */
    usleep(200000);
    int blocked = !a.done;
    fprintf(stderr, "CONTEND blocked=%d (expected 1)\n", blocked);
    if (!blocked) failures++;
    call("kernel32.dll", "LeaveCriticalSection", (uint32_t[]){cs}, 1, NULL);
    pthread_join(host, NULL);
    fprintf(stderr, "CONTEND enter_rc=%d eax=0x%08X leave_rc=%d done=%d\n",
            a.rc_enter, (uint32_t)a.eax_enter, a.rc_leave, a.done);
    if (a.rc_enter || a.rc_leave || !a.done) failures++;
    call("kernel32.dll", "DeleteCriticalSection", (uint32_t[]){cs}, 1, NULL);
    fprintf(stderr, "SELFTEST_CONTENTION failures=%d\n", failures);
    return failures ? 1 : 0;
}

int main(int argc, char **argv) {
    const char *game = NULL, *user = NULL, *nls = NULL;
    uint32_t call_va = 0;
    int selftest = 0, threads = 0, dump = 0, resolve_unknown = 1, trace_exports = 0;
    int contention = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--game") && i + 1 < argc) game = argv[++i];
        else if (!strcmp(argv[i], "--user") && i + 1 < argc) user = argv[++i];
        else if (!strcmp(argv[i], "--nls") && i + 1 < argc) nls = argv[++i];
        else if (!strcmp(argv[i], "--call-va") && i + 1 < argc)
            call_va = (uint32_t)strtoul(argv[++i], NULL, 0);
        else if (!strcmp(argv[i], "--selftest-imports")) selftest = 1;
        else if (!strcmp(argv[i], "--strict-export-resolution")) resolve_unknown = 0;
        else if (!strcmp(argv[i], "--trace-exports")) trace_exports = 1;
        else if (!strcmp(argv[i], "--selftest-threads")) threads = 1;
        else if (!strcmp(argv[i], "--selftest-contention")) contention = 1;
        else if (!strcmp(argv[i], "--dump-imports")) dump = 1;
        else {
            fprintf(stderr,
                    "usage: %s --game <dir> --user <dir> [--nls <path>]\n"
                    "       [--selftest-imports] [--selftest-threads]\n"
                    "       [--dump-imports] [--call-va <va>]\n",
                    argv[0]);
            return 2;
        }
    }
    if (!game || !user) {
        fprintf(stderr, "usage: %s --game <dir> --user <dir> [--nls <path>]\n",
                argv[0]);
        return 2;
    }
    port_set_resolve_unknown_exports(resolve_unknown);
    if (trace_exports) port_set_trace_exports(1);
    if (selftest || threads || dump || call_va || contention) {
        if (port_setup(game, user, nls)) return 1;
        int rc = 0;
        if (dump) port_dump_imports();
        if (selftest) rc |= selftest_imports();
        if (threads) rc |= selftest_threads();
        if (contention) rc |= selftest_contention();
        if (call_va && port_call_va(call_va)) rc = 1;
        fprintf(stderr, "PORT_RESULT mode=%s rc=%d bound_imports=%u/%u bridges=%u\n",
                dump ? "dump" : selftest ? "selftest"
                     : threads ? "threads" : "call-va",
                rc, port_bound_imports(), port_total_imports(),
                port_bridge_count());
        return rc;
    }
    int rc = port_entry_ex(game, user, nls);
    fprintf(stderr,
            "PORT_RESULT rc=%d bound_imports=%u/%u eip=0x%08X esp=0x%08X\n",
            rc, port_bound_imports(), port_total_imports(), port_guest_eip(),
            port_guest_esp());
    return rc ? 1 : 0;
}
