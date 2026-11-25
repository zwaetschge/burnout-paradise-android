/* ws2_32.dll for the flat-model static engine: every socket call goes to the
 * host BSD socket layer and the result is the host's result.
 *
 * Nothing is simulated: no connection, packet or name is invented. A machine
 * without a route simply sees the errors a Windows machine without a route
 * sees (WSAHOST_NOT_FOUND, WSAENETUNREACH ...). PORT_OFFLINE=1 models exactly
 * that -- no network interface -- for reproducible development runs: name
 * resolution finds nothing and non-loopback destinations are unreachable.
 *
 * Differences that need translating (and are translated here):
 *   - error numbers (errno -> WSAE*), reported through the calling thread's TEB
 *   - SOL_SOCKET / option numbers, struct linger, millisecond timeouts
 *   - fd_set (count + array of SOCKETs), struct hostent / addrinfo with 32-bit
 *     pointers (built in guest memory)
 *   - WSA events are ordinary manual-reset events of the sync-object model
 * Overlapped receives complete on a helper thread (event signalled, as on
 * Windows); completion routines (APCs) and IPv6 are not provided (named stops).
 *
 * Blocking calls release the guest machine lock while they wait. */
#define _GNU_SOURCE                                 /* pipe2, accept4 */
#include "recomp_types.h"
#include "static_bridges.h"
#include "static_threads.h"
#include "guest_sync_objects.h"
#include "orig_bridges.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <unistd.h>

uint8_t *port_flat_base(void);
uint32_t d3d9t_guest_alloc(uint32_t size);          /* guest scratch (lock arena) */
void d3d9t_guest_free(uint32_t address);
int sb_close_handle_public(uint32_t handle, uint32_t *result);

#define GA(i) (a[(i)])
#define G(va) (port_flat_base() + (va))
#define SOCKET_ERROR_ 0xFFFFFFFFu
#define INVALID_SOCKET_ 0xFFFFFFFFu
#define SOCK_BASE 0x00000A00u
#define SOCK_MAX 256

enum { WSAEINTR_ = 10004, WSAEBADF_ = 10009, WSAEACCES_ = 10013, WSAEFAULT_ = 10014, WSAEINVAL_ = 10022, WSAEMFILE_ = 10024,
       WSAEWOULDBLOCK_ = 10035, WSAEINPROGRESS_ = 10036, WSAEALREADY_ = 10037, WSAENOTSOCK_ = 10038, WSAEDESTADDRREQ_ = 10039,
       WSAEMSGSIZE_ = 10040, WSAEPROTOTYPE_ = 10041, WSAENOPROTOOPT_ = 10042, WSAEPROTONOSUPPORT_ = 10043,
       WSAESOCKTNOSUPPORT_ = 10044, WSAEOPNOTSUPP_ = 10045, WSAEAFNOSUPPORT_ = 10047, WSAEADDRINUSE_ = 10048,
       WSAEADDRNOTAVAIL_ = 10049, WSAENETDOWN_ = 10050, WSAENETUNREACH_ = 10051, WSAENETRESET_ = 10052,
       WSAECONNABORTED_ = 10053, WSAECONNRESET_ = 10054, WSAENOBUFS_ = 10055, WSAEISCONN_ = 10056, WSAENOTCONN_ = 10057,
       WSAESHUTDOWN_ = 10058, WSAETIMEDOUT_ = 10060, WSAECONNREFUSED_ = 10061, WSAEHOSTUNREACH_ = 10065,
       WSAHOST_NOT_FOUND_ = 11001, WSATRY_AGAIN_ = 11002, WSANO_RECOVERY_ = 11003, WSANO_DATA_ = 11004,
       WSA_NOT_ENOUGH_MEMORY_ = 8 };

static struct { int fd; int used; int nonblocking; } g_sock[SOCK_MAX];
static pthread_mutex_t g_sock_lock = PTHREAD_MUTEX_INITIALIZER;

static void set_error(uint32_t e) { memcpy(G(g_teb + 0x34), &e, 4); }
static int offline(void) {
    static int value = -1;
    if (value < 0) { const char *e = getenv("PORT_OFFLINE"); value = e && *e && *e != '0'; }
    return value;
}
static int trace(void) {
    static int value = -1;
    if (value < 0) value = getenv("PORT_TRACE_NET") ? 1 : 0;
    return value;
}
static uint32_t map_errno(int e) {
    switch (e) {
    case 0: return 0;
    case EINTR: return WSAEINTR_; case EBADF: return WSAEBADF_; case EACCES: case EPERM: return WSAEACCES_;
    case EFAULT: return WSAEFAULT_; case EINVAL: return WSAEINVAL_; case EMFILE: case ENFILE: return WSAEMFILE_;
    case EAGAIN: return WSAEWOULDBLOCK_; case EINPROGRESS: return WSAEWOULDBLOCK_; case EALREADY: return WSAEALREADY_;
    case ENOTSOCK: return WSAENOTSOCK_; case EDESTADDRREQ: return WSAEDESTADDRREQ_; case EMSGSIZE: return WSAEMSGSIZE_;
    case EPROTOTYPE: return WSAEPROTOTYPE_; case ENOPROTOOPT: return WSAENOPROTOOPT_;
    case EPROTONOSUPPORT: return WSAEPROTONOSUPPORT_; case ESOCKTNOSUPPORT: return WSAESOCKTNOSUPPORT_;
    case EOPNOTSUPP: return WSAEOPNOTSUPP_; case EAFNOSUPPORT: return WSAEAFNOSUPPORT_; case EADDRINUSE: return WSAEADDRINUSE_;
    case EADDRNOTAVAIL: return WSAEADDRNOTAVAIL_; case ENETDOWN: return WSAENETDOWN_; case ENETUNREACH: return WSAENETUNREACH_;
    case ENETRESET: return WSAENETRESET_; case ECONNABORTED: return WSAECONNABORTED_; case ECONNRESET: case EPIPE: return WSAECONNRESET_;
    case ENOBUFS: case ENOMEM: return WSAENOBUFS_; case EISCONN: return WSAEISCONN_; case ENOTCONN: return WSAENOTCONN_;
    case ESHUTDOWN: return WSAESHUTDOWN_; case ETIMEDOUT: return WSAETIMEDOUT_; case ECONNREFUSED: return WSAECONNREFUSED_;
    case EHOSTUNREACH: return WSAEHOSTUNREACH_;
    default: return WSAEINVAL_;
    }
}
static int fd_of(uint32_t s) {
    uint32_t i = (s - SOCK_BASE) / 4u;
    if (s < SOCK_BASE || ((s - SOCK_BASE) & 3u) || i >= SOCK_MAX || !g_sock[i].used) return -1;
    return g_sock[i].fd;
}
static uint32_t new_socket(int fd) {
    pthread_mutex_lock(&g_sock_lock);
    for (uint32_t i = 0; i < SOCK_MAX; i++)
        if (!g_sock[i].used) {
            g_sock[i].used = 1; g_sock[i].fd = fd; g_sock[i].nonblocking = 0;
            pthread_mutex_unlock(&g_sock_lock);
            return SOCK_BASE + i * 4u;
        }
    pthread_mutex_unlock(&g_sock_lock);
    return INVALID_SOCKET_;
}
/* Windows sockaddr_in and the host's are the same 16 bytes (family is a
 * little-endian u16, AF_INET = 2 on both). Only AF_INET is accepted. */
static int addr_in(uint32_t va, uint32_t len, struct sockaddr_in *out) {
    if (!va || len < 16) return WSAEFAULT_;
    uint16_t family; memcpy(&family, G(va), 2);
    if (family != 2) return WSAEAFNOSUPPORT_;
    memset(out, 0, sizeof *out);
    out->sin_family = AF_INET;
    memcpy(&out->sin_port, G(va + 2), 2); memcpy(&out->sin_addr, G(va + 4), 4);
    return 0;
}
static void addr_out(uint32_t va, uint32_t len_va, const struct sockaddr_in *in) {
    if (!va || !len_va) return;
    uint32_t cap; memcpy(&cap, G(len_va), 4);
    uint8_t raw[16]; memset(raw, 0, sizeof raw);
    raw[0] = 2; memcpy(raw + 2, &in->sin_port, 2); memcpy(raw + 4, &in->sin_addr, 4);
    memcpy(G(va), raw, cap < 16 ? cap : 16);
    cap = 16; memcpy(G(len_va), &cap, 4);
}
static int unreachable(const struct sockaddr_in *to) {      /* PORT_OFFLINE: only loopback is routable */
    return offline() && (ntohl(to->sin_addr.s_addr) >> 24) != 127;
}
#define FAIL(code) do { set_error(code); *r = SOCKET_ERROR_; return GM_OK; } while (0)

/* ---- byte order ---- */
static int ws_htons(const uint32_t *a, uint32_t *r) { *r = htons((uint16_t)GA(0)); return GM_OK; }
static int ws_ntohs(const uint32_t *a, uint32_t *r) { *r = ntohs((uint16_t)GA(0)); return GM_OK; }
static int ws_htonl(const uint32_t *a, uint32_t *r) { *r = htonl(GA(0)); return GM_OK; }
static int ws_ntohl(const uint32_t *a, uint32_t *r) { *r = ntohl(GA(0)); return GM_OK; }
static int ws_get_last_error(const uint32_t *a, uint32_t *r) { (void)a; memcpy(r, G(g_teb + 0x34), 4); return GM_OK; }

static void abort_pending(uint32_t sock);

/* ---- sockets ---- */
static int ws_socket(const uint32_t *a, uint32_t *r) {      /* (af, type, protocol) */
    *r = INVALID_SOCKET_;
    if (GA(0) != 2) { set_error(WSAEAFNOSUPPORT_); return GM_OK; }
    if (GA(1) != 1 && GA(1) != 2) { set_error(WSAESOCKTNOSUPPORT_); return GM_OK; }
    int fd = socket(AF_INET, (GA(1) == 1 ? SOCK_STREAM : SOCK_DGRAM) | SOCK_CLOEXEC, (int)GA(2));
    if (fd < 0) { set_error(map_errno(errno)); return GM_OK; }
    uint32_t s = new_socket(fd);
    if (s == INVALID_SOCKET_) { close(fd); set_error(WSAEMFILE_); return GM_OK; }
    if (trace()) fprintf(stderr, "PORT_NET socket(type=%u proto=%u) -> 0x%X\n", GA(1), GA(2), s);
    *r = s;
    return GM_OK;
}
static int ws_closesocket(const uint32_t *a, uint32_t *r) {
    int fd = fd_of(GA(0));
    if (fd < 0) FAIL(WSAENOTSOCK_);
    abort_pending(GA(0));
    pthread_mutex_lock(&g_sock_lock);
    g_sock[(GA(0) - SOCK_BASE) / 4u].used = 0;
    pthread_mutex_unlock(&g_sock_lock);
    close(fd);
    *r = 0;
    return GM_OK;
}
static int ws_shutdown(const uint32_t *a, uint32_t *r) {
    int fd = fd_of(GA(0));
    if (fd < 0) FAIL(WSAENOTSOCK_);
    if (shutdown(fd, (int)GA(1))) FAIL(map_errno(errno));   /* SD_RECEIVE/SEND/BOTH = SHUT_RD/WR/RDWR */
    *r = 0;
    return GM_OK;
}
static int ws_bind(const uint32_t *a, uint32_t *r) {        /* (s, name, namelen) */
    int fd = fd_of(GA(0)); struct sockaddr_in sa;
    if (fd < 0) FAIL(WSAENOTSOCK_);
    int e = addr_in(GA(1), GA(2), &sa);
    if (e) FAIL((uint32_t)e);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa)) FAIL(map_errno(errno));
    if (trace()) fprintf(stderr, "PORT_NET bind(0x%X, %s:%u)\n", GA(0), inet_ntoa(sa.sin_addr), ntohs(sa.sin_port));
    *r = 0;
    return GM_OK;
}
static int ws_connect(const uint32_t *a, uint32_t *r) {
    int fd = fd_of(GA(0)); struct sockaddr_in sa;
    if (fd < 0) FAIL(WSAENOTSOCK_);
    int e = addr_in(GA(1), GA(2), &sa);
    if (e) FAIL((uint32_t)e);
    if (trace()) fprintf(stderr, "PORT_NET connect(0x%X, %s:%u)%s\n", GA(0), inet_ntoa(sa.sin_addr), ntohs(sa.sin_port), unreachable(&sa) ? " -> no route (PORT_OFFLINE)" : "");
    if (unreachable(&sa)) FAIL(WSAENETUNREACH_);
    int held = st_machine_release();
    int rc = connect(fd, (struct sockaddr *)&sa, sizeof sa), err = errno;
    st_machine_reacquire(held);
    if (rc) FAIL(map_errno(err));
    *r = 0;
    return GM_OK;
}
static int ws_accept(const uint32_t *a, uint32_t *r) {      /* (s, addr, addrlen) */
    int fd = fd_of(GA(0));
    *r = INVALID_SOCKET_;
    if (fd < 0) { set_error(WSAENOTSOCK_); return GM_OK; }
    struct sockaddr_in sa; socklen_t len = sizeof sa;
    int held = st_machine_release();
    int nfd = accept4(fd, (struct sockaddr *)&sa, &len, SOCK_CLOEXEC), err = errno;
    st_machine_reacquire(held);
    if (nfd < 0) { set_error(map_errno(err)); return GM_OK; }
    uint32_t s = new_socket(nfd);
    if (s == INVALID_SOCKET_) { close(nfd); set_error(WSAEMFILE_); return GM_OK; }
    addr_out(GA(1), GA(2), &sa);
    *r = s;
    return GM_OK;
}
static int name_call(const uint32_t *a, uint32_t *r, int peer) {
    int fd = fd_of(GA(0));
    if (fd < 0) FAIL(WSAENOTSOCK_);
    if (!GA(1) || !GA(2)) FAIL(WSAEFAULT_);
    struct sockaddr_in sa; socklen_t len = sizeof sa;
    memset(&sa, 0, sizeof sa);
    if ((peer ? getpeername : getsockname)(fd, (struct sockaddr *)&sa, &len)) FAIL(map_errno(errno));
    addr_out(GA(1), GA(2), &sa);
    *r = 0;
    return GM_OK;
}
static int ws_getsockname(const uint32_t *a, uint32_t *r) { return name_call(a, r, 0); }
static int ws_getpeername(const uint32_t *a, uint32_t *r) { return name_call(a, r, 1); }

/* MSG_OOB (1) and MSG_PEEK (2) have the host's values; nothing else is passed on. */
static int io_flags(uint32_t flags, int *out) {
    if (flags & ~3u) return -1;
    *out = (int)flags | MSG_NOSIGNAL;
    return 0;
}
static int ws_send(const uint32_t *a, uint32_t *r) {        /* (s, buf, len, flags) */
    int fd = fd_of(GA(0)), flags;
    if (fd < 0) FAIL(WSAENOTSOCK_);
    if (io_flags(GA(3), &flags)) FAIL(WSAEOPNOTSUPP_);
    int held = st_machine_release();
    ssize_t n = send(fd, G(GA(1)), GA(2), flags); int err = errno;
    st_machine_reacquire(held);
    if (n < 0) FAIL(map_errno(err));
    *r = (uint32_t)n;
    return GM_OK;
}
static int ws_recv(const uint32_t *a, uint32_t *r) {
    int fd = fd_of(GA(0)), flags;
    if (fd < 0) FAIL(WSAENOTSOCK_);
    if (io_flags(GA(3), &flags)) FAIL(WSAEOPNOTSUPP_);
    int held = st_machine_release();
    ssize_t n = recv(fd, G(GA(1)), GA(2), flags); int err = errno;
    st_machine_reacquire(held);
    if (n < 0) FAIL(map_errno(err));
    *r = (uint32_t)n;
    return GM_OK;
}
static int ws_sendto(const uint32_t *a, uint32_t *r) {      /* (s, buf, len, flags, to, tolen) */
    int fd = fd_of(GA(0)), flags; struct sockaddr_in sa;
    if (fd < 0) FAIL(WSAENOTSOCK_);
    if (io_flags(GA(3), &flags)) FAIL(WSAEOPNOTSUPP_);
    ssize_t n; int err;
    if (GA(4)) {
        int e = addr_in(GA(4), GA(5), &sa);
        if (e) FAIL((uint32_t)e);
        if (trace()) fprintf(stderr, "PORT_NET sendto(0x%X, %u bytes, %s:%u)%s\n", GA(0), GA(2), inet_ntoa(sa.sin_addr), ntohs(sa.sin_port), unreachable(&sa) ? " -> no route (PORT_OFFLINE)" : "");
        if (unreachable(&sa)) FAIL(WSAENETUNREACH_);
        n = sendto(fd, G(GA(1)), GA(2), flags, (struct sockaddr *)&sa, sizeof sa); err = errno;
    } else { n = send(fd, G(GA(1)), GA(2), flags); err = errno; }
    if (n < 0) FAIL(map_errno(err));
    *r = (uint32_t)n;
    return GM_OK;
}
static int ws_recvfrom(const uint32_t *a, uint32_t *r) {    /* (s, buf, len, flags, from, fromlen) */
    int fd = fd_of(GA(0)), flags;
    if (fd < 0) FAIL(WSAENOTSOCK_);
    if (io_flags(GA(3), &flags)) FAIL(WSAEOPNOTSUPP_);
    struct sockaddr_in sa; socklen_t len = sizeof sa;
    memset(&sa, 0, sizeof sa);
    int held = st_machine_release();
    ssize_t n = recvfrom(fd, G(GA(1)), GA(2), flags, (struct sockaddr *)&sa, &len); int err = errno;
    st_machine_reacquire(held);
    if (n < 0) FAIL(map_errno(err));
    addr_out(GA(4), GA(5), &sa);
    *r = (uint32_t)n;
    return GM_OK;
}

/* ---- WSARecv / WSARecvFrom, including overlapped receives ---- */
/* An overlapped receive that cannot complete at once is parked here; the
 * completion thread waits for the socket to become readable, performs the
 * receive into the guest's buffers, stores the result in the WSAOVERLAPPED
 * (Internal = NTSTATUS, InternalHigh = bytes, as Windows does) and signals
 * hEvent. Completion routines (APCs) are not provided. */
int port_set_guest_event(uint32_t scratch_teb, uint32_t handle);
#define STATUS_PENDING_ 0x00000103u
#define STATUS_CANCELLED_ 0xC0000120u
#define WSA_IO_PENDING_ 997u
#define WSA_IO_INCOMPLETE_ 996u
#define WSA_OPERATION_ABORTED_ 995u
typedef struct { int used; uint32_t sock; int fd; uint32_t bufs, nbufs, flags_va, from, fromlen, ovl, event; } Pending;
static Pending g_pending[64];
static pthread_mutex_t g_pending_lock = PTHREAD_MUTEX_INITIALIZER;
static int g_wake[2] = { -1, -1 };
static uint32_t g_completion_teb;

static int gather(uint32_t bufs, uint32_t count, struct iovec *iov) {
    for (uint32_t i = 0; i < count; i++) {                  /* WSABUF { ULONG len; CHAR *buf; } */
        uint32_t len, buf; memcpy(&len, G(bufs + i * 8), 4); memcpy(&buf, G(bufs + i * 8 + 4), 4);
        iov[i].iov_base = G(buf); iov[i].iov_len = len;
    }
    return 0;
}
/* Stores the outcome of an overlapped receive and signals its event. */
static void complete(Pending *p, uint32_t status, uint32_t bytes, const struct sockaddr_in *from) {
    if (from && p->from) addr_out(p->from, p->fromlen, from);
    if (p->flags_va) { uint32_t zero = 0; memcpy(G(p->flags_va), &zero, 4); }
    memcpy(G(p->ovl + 4), &bytes, 4);
    memcpy(G(p->ovl), &status, 4);
    if (p->event) port_set_guest_event(g_completion_teb, p->event);
    p->used = 0;
}
static void *completion_thread(void *unused) {
    (void)unused;
    pthread_setname_np(pthread_self(), "port-wsa");
    for (;;) {
        struct pollfd fds[65]; int index[65], count = 1;
        fds[0].fd = g_wake[0]; fds[0].events = POLLIN; fds[0].revents = 0;
        pthread_mutex_lock(&g_pending_lock);
        for (int i = 0; i < 64; i++)
            if (g_pending[i].used) { fds[count].fd = g_pending[i].fd; fds[count].events = POLLIN; fds[count].revents = 0; index[count] = i; count++; }
        pthread_mutex_unlock(&g_pending_lock);
        if (poll(fds, (nfds_t)count, -1) < 0) continue;
        if (fds[0].revents) { char drain[64]; while (read(g_wake[0], drain, sizeof drain) > 0) { } }
        pthread_mutex_lock(&g_pending_lock);
        for (int k = 1; k < count; k++) {
            Pending *p = &g_pending[index[k]];
            if (!p->used || p->fd != fds[k].fd || !fds[k].revents) continue;
            struct iovec iov[16]; gather(p->bufs, p->nbufs, iov);
            struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
            struct msghdr msg; memset(&msg, 0, sizeof msg);
            msg.msg_iov = iov; msg.msg_iovlen = p->nbufs; msg.msg_name = &sa; msg.msg_namelen = sizeof sa;
            ssize_t n = recvmsg(p->fd, &msg, MSG_DONTWAIT);
            if (n < 0 && (errno == EAGAIN || errno == EINTR)) continue;
            if (n < 0) complete(p, 0xC0000000u | map_errno(errno), 0, NULL);
            else complete(p, 0, (uint32_t)n, &sa);
        }
        pthread_mutex_unlock(&g_pending_lock);
    }
    return NULL;
}
static void completion_start(void) {
    pthread_mutex_lock(&g_pending_lock);
    if (g_wake[0] < 0 && !pipe2(g_wake, O_CLOEXEC | O_NONBLOCK)) {
        g_completion_teb = d3d9t_guest_alloc(0x1000);
        pthread_t thread;
        pthread_create(&thread, NULL, completion_thread, NULL);
        pthread_detach(thread);
    }
    pthread_mutex_unlock(&g_pending_lock);
}
/* Pending receives of a socket that is being closed end as aborted. */
static void abort_pending(uint32_t sock) {
    pthread_mutex_lock(&g_pending_lock);
    for (int i = 0; i < 64; i++)
        if (g_pending[i].used && g_pending[i].sock == sock) complete(&g_pending[i], STATUS_CANCELLED_, 0, NULL);
    pthread_mutex_unlock(&g_pending_lock);
    if (g_wake[1] >= 0 && write(g_wake[1], "x", 1) < 0) { }
}
static int wsa_recv_common(const uint32_t *a, uint32_t *r, uint32_t from, uint32_t fromlen, uint32_t overlapped, uint32_t completion) {
    /* (s, lpBuffers, dwBufferCount, lpNumberOfBytesRecvd, lpFlags, ...) */
    int fd = fd_of(GA(0));
    if (fd < 0) FAIL(WSAENOTSOCK_);
    if (completion) { fprintf(stderr, "PORT_NET socket completion routines (APC) are not provided\n"); return GM_UNSUPPORTED; }
    if (!GA(1) || !GA(2) || GA(2) > 16 || !GA(4) || (!GA(3) && !overlapped)) FAIL(WSAEFAULT_);
    struct iovec iov[16]; gather(GA(1), GA(2), iov);
    uint32_t wflags; memcpy(&wflags, G(GA(4)), 4);
    int flags;
    if (io_flags(wflags, &flags)) FAIL(WSAEOPNOTSUPP_);
    struct sockaddr_in sa; memset(&sa, 0, sizeof sa);
    struct msghdr msg; memset(&msg, 0, sizeof msg);
    msg.msg_iov = iov; msg.msg_iovlen = GA(2);
    if (from) { msg.msg_name = &sa; msg.msg_namelen = sizeof sa; }
    ssize_t n; int err;
    if (overlapped) {
        uint32_t event; memcpy(&event, G(overlapped + 16), 4);
        if (event) { uint32_t ignored; gso_reset_event(g_mem, g_teb, event, &ignored); }   /* the system resets it when the operation starts */
        n = recvmsg(fd, &msg, flags | MSG_DONTWAIT); err = errno;
        if (n < 0 && err == EAGAIN) {
            completion_start();
            pthread_mutex_lock(&g_pending_lock);
            Pending *p = NULL;
            for (int i = 0; i < 64 && !p; i++) if (!g_pending[i].used) p = &g_pending[i];
            if (p) {
                p->used = 1; p->sock = GA(0); p->fd = fd; p->bufs = GA(1); p->nbufs = GA(2); p->flags_va = GA(4);
                p->from = from; p->fromlen = fromlen; p->ovl = overlapped; p->event = event;
                uint32_t pending = STATUS_PENDING_, zero = 0;
                memcpy(G(overlapped), &pending, 4); memcpy(G(overlapped + 4), &zero, 4);
            }
            pthread_mutex_unlock(&g_pending_lock);
            if (!p) FAIL(WSAENOBUFS_);
            if (write(g_wake[1], "x", 1) < 0) { }
            FAIL(WSA_IO_PENDING_);
        }
        if (n >= 0) {                                       /* completed at once: result stored and event signalled */
            uint32_t got = (uint32_t)n, zero = 0;
            memcpy(G(overlapped), &zero, 4); memcpy(G(overlapped + 4), &got, 4);
            if (event) { uint32_t ignored; gso_set_event(g_mem, g_teb, event, &ignored); }
        }
    } else {
        int held = st_machine_release();
        n = recvmsg(fd, &msg, flags); err = errno;
        st_machine_reacquire(held);
    }
    if (n < 0) FAIL(map_errno(err));
    uint32_t got = (uint32_t)n, zero = 0;
    if (GA(3)) memcpy(G(GA(3)), &got, 4);
    memcpy(G(GA(4)), &zero, 4);
    if (from) addr_out(from, fromlen, &sa);
    *r = 0;
    return GM_OK;
}
static int ws_wsa_recv(const uint32_t *a, uint32_t *r) { return wsa_recv_common(a, r, 0, 0, GA(5), GA(6)); }
static int ws_wsa_recv_from(const uint32_t *a, uint32_t *r) { return wsa_recv_common(a, r, GA(5), GA(6), GA(7), GA(8)); }
static int ws_wsa_get_overlapped_result(const uint32_t *a, uint32_t *r) {   /* (s, lpOverlapped, lpcbTransfer, fWait, lpdwFlags) */
    *r = 0;
    if (fd_of(GA(0)) < 0) { set_error(WSAENOTSOCK_); return GM_OK; }
    if (!GA(1) || !GA(2) || !GA(4)) { set_error(WSAEFAULT_); return GM_OK; }
    uint32_t status; memcpy(&status, G(GA(1)), 4);
    if (status == STATUS_PENDING_ && GA(3)) {
        uint32_t event; memcpy(&event, G(GA(1) + 16), 4);
        if (!event) { fprintf(stderr, "PORT_NET WSAGetOverlappedResult(fWait) without an event\n"); return GM_UNSUPPORTED; }
        int held = st_machine_release();
        uint32_t waited = 0;
        int rc = gso_wait_single(g_mem, g_teb, event, 0xFFFFFFFFu, &waited);
        st_machine_reacquire(held);
        if (rc) return rc;
        memcpy(&status, G(GA(1)), 4);
    }
    if (status == STATUS_PENDING_) { set_error(WSA_IO_INCOMPLETE_); return GM_OK; }
    if (status == STATUS_CANCELLED_) { set_error(WSA_OPERATION_ABORTED_); return GM_OK; }
    if (status) { set_error(status & 0xFFFFu); return GM_OK; }
    uint32_t bytes, zero = 0; memcpy(&bytes, G(GA(1) + 4), 4);
    memcpy(G(GA(2)), &bytes, 4); memcpy(G(GA(4)), &zero, 4);
    *r = 1;
    return GM_OK;
}

/* ---- options ---- */
static int option_map(uint32_t level, uint32_t name, int *hl, int *hn, int *kind) {
    *kind = 0;                                              /* 0 int/BOOL, 1 linger, 2 timeout (ms) */
    if (level == 0xFFFFu) {                                 /* SOL_SOCKET */
        *hl = SOL_SOCKET;
        switch (name) {
        case 0x0004: *hn = SO_REUSEADDR; return 0;
        case 0x0008: *hn = SO_KEEPALIVE; return 0;
        case 0x0020: *hn = SO_BROADCAST; return 0;
        case 0x0080: *hn = SO_LINGER; *kind = 1; return 0;
        case 0x0100: *hn = SO_OOBINLINE; return 0;
        case 0x1001: *hn = SO_SNDBUF; return 0;
        case 0x1002: *hn = SO_RCVBUF; return 0;
        case 0x1005: *hn = SO_SNDTIMEO; *kind = 2; return 0;
        case 0x1006: *hn = SO_RCVTIMEO; *kind = 2; return 0;
        case 0x1007: *hn = SO_ERROR; return 0;
        case 0x1008: *hn = SO_TYPE; return 0;
        default: return -1;
        }
    }
    if (level == 6 && name == 1) { *hl = IPPROTO_TCP; *hn = TCP_NODELAY; return 0; }
    if (level == 0) {                                       /* IPPROTO_IP */
        *hl = IPPROTO_IP;
        switch (name) {
        case 3: *hn = IP_TOS; return 0;
        case 4: *hn = IP_TTL; return 0;
        default: return -1;
        }
    }
    return -1;
}
static int ws_setsockopt(const uint32_t *a, uint32_t *r) {  /* (s, level, optname, optval, optlen) */
    int fd = fd_of(GA(0)), hl, hn, kind;
    if (fd < 0) FAIL(WSAENOTSOCK_);
    if (option_map(GA(1), GA(2), &hl, &hn, &kind)) {
        fprintf(stderr, "PORT_NET setsockopt level=0x%X option=0x%X is not mapped\n", GA(1), GA(2));
        return GM_UNSUPPORTED;
    }
    if (!GA(3)) FAIL(WSAEFAULT_);
    int rc;
    if (kind == 1) {
        if (GA(4) < 4) FAIL(WSAEFAULT_);
        uint16_t w[2]; memcpy(w, G(GA(3)), 4);
        struct linger l = { w[0], w[1] };
        rc = setsockopt(fd, hl, hn, &l, sizeof l);
    } else if (kind == 2) {
        if (GA(4) < 4) FAIL(WSAEFAULT_);
        uint32_t ms; memcpy(&ms, G(GA(3)), 4);
        struct timeval tv = { (time_t)(ms / 1000u), (suseconds_t)((ms % 1000u) * 1000u) };
        rc = setsockopt(fd, hl, hn, &tv, sizeof tv);
    } else {
        int value = 0;
        if (GA(4) >= 4) memcpy(&value, G(GA(3)), 4);
        else if (GA(4) >= 1) value = *G(GA(3));
        else FAIL(WSAEFAULT_);
        rc = setsockopt(fd, hl, hn, &value, sizeof value);
    }
    if (rc) FAIL(map_errno(errno));
    *r = 0;
    return GM_OK;
}
static int ws_getsockopt(const uint32_t *a, uint32_t *r) {  /* (s, level, optname, optval, optlen*) */
    int fd = fd_of(GA(0)), hl, hn, kind;
    if (fd < 0) FAIL(WSAENOTSOCK_);
    if (option_map(GA(1), GA(2), &hl, &hn, &kind)) {
        fprintf(stderr, "PORT_NET getsockopt level=0x%X option=0x%X is not mapped\n", GA(1), GA(2));
        return GM_UNSUPPORTED;
    }
    if (!GA(3) || !GA(4)) FAIL(WSAEFAULT_);
    uint32_t cap; memcpy(&cap, G(GA(4)), 4);
    if (cap < 4) FAIL(WSAEFAULT_);
    uint32_t out = 0;
    if (kind == 1) {
        struct linger l; socklen_t len = sizeof l;
        if (getsockopt(fd, hl, hn, &l, &len)) FAIL(map_errno(errno));
        uint16_t w[2] = { (uint16_t)l.l_onoff, (uint16_t)l.l_linger }; memcpy(&out, w, 4);
    } else if (kind == 2) {
        struct timeval tv; socklen_t len = sizeof tv;
        if (getsockopt(fd, hl, hn, &tv, &len)) FAIL(map_errno(errno));
        out = (uint32_t)(tv.tv_sec * 1000 + tv.tv_usec / 1000);
    } else {
        int value = 0; socklen_t len = sizeof value;
        if (getsockopt(fd, hl, hn, &value, &len)) FAIL(map_errno(errno));
        if (hl == SOL_SOCKET && hn == SO_ERROR) value = (int)map_errno(value);
        if (hl == SOL_SOCKET && hn == SO_TYPE) value = value == SOCK_STREAM ? 1 : 2;
        if (hl == SOL_SOCKET && (hn == SO_SNDBUF || hn == SO_RCVBUF)) value /= 2;   /* Linux reports the doubled size */
        out = (uint32_t)value;
    }
    memcpy(G(GA(3)), &out, 4);
    cap = 4; memcpy(G(GA(4)), &cap, 4);
    *r = 0;
    return GM_OK;
}
static int ws_ioctlsocket(const uint32_t *a, uint32_t *r) { /* (s, cmd, argp) */
    int fd = fd_of(GA(0));
    if (fd < 0) FAIL(WSAENOTSOCK_);
    if (!GA(2)) FAIL(WSAEFAULT_);
    uint32_t arg; memcpy(&arg, G(GA(2)), 4);
    if (GA(1) == 0x8004667Eu) {                             /* FIONBIO */
        int flags = fcntl(fd, F_GETFL, 0);
        if (flags < 0 || fcntl(fd, F_SETFL, arg ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK))) FAIL(map_errno(errno));
        g_sock[(GA(0) - SOCK_BASE) / 4u].nonblocking = arg != 0;
        *r = 0;
        return GM_OK;
    }
    if (GA(1) == 0x4004667Fu) {                             /* FIONREAD */
        int n = 0;
        if (ioctl(fd, FIONREAD, &n)) FAIL(map_errno(errno));
        arg = (uint32_t)n; memcpy(G(GA(2)), &arg, 4);
        *r = 0;
        return GM_OK;
    }
    fprintf(stderr, "PORT_NET ioctlsocket command 0x%08X is not mapped\n", GA(1));
    return GM_UNSUPPORTED;
}
static int ws_wsa_ioctl(const uint32_t *a, uint32_t *r) {   /* (s, code, in, cbIn, out, cbOut, lpcbRet, ovl, completion) */
    (void)r;
    fprintf(stderr, "PORT_NET WSAIoctl control code 0x%08X (in %u bytes, out %u bytes) is not mapped\n", GA(1), GA(3), GA(5));
    return GM_UNSUPPORTED;
}

/* ---- select ---- */
/* fd_set: u_int fd_count; SOCKET fd_array[64]. timeval: two 32-bit longs. */
static int ws_select(const uint32_t *a, uint32_t *r) {      /* (nfds, read, write, except, timeout) */
    struct pollfd fds[192]; uint32_t sock[192]; uint8_t want[192]; int count = 0;
    for (int set = 0; set < 3; set++) {
        uint32_t va = GA(1 + set);
        if (!va) continue;
        uint32_t n; memcpy(&n, G(va), 4);
        if (n > 64) FAIL(WSAEINVAL_);
        for (uint32_t i = 0; i < n; i++) {
            uint32_t s; memcpy(&s, G(va + 4 + i * 4), 4);
            int fd = fd_of(s);
            if (fd < 0) FAIL(WSAENOTSOCK_);
            int k = 0;
            for (; k < count; k++) if (sock[k] == s) break;
            if (k == count) { fds[k].fd = fd; fds[k].events = 0; fds[k].revents = 0; sock[k] = s; want[k] = 0; count++; }
            want[k] |= (uint8_t)(1 << set);
            fds[k].events |= set == 0 ? POLLIN : (set == 1 ? POLLOUT : POLLPRI);
        }
    }
    if (!count) FAIL(WSAEINVAL_);                           /* Winsock: no descriptor sets at all */
    int timeout = -1;
    if (GA(4)) { int32_t tv[2]; memcpy(tv, G(GA(4)), 8); timeout = tv[0] * 1000 + tv[1] / 1000; if (timeout < 0) timeout = 0; }
    int held = timeout ? st_machine_release() : 0;
    int rc = poll(fds, (nfds_t)count, timeout), err = errno;
    if (timeout) st_machine_reacquire(held);
    if (rc < 0) FAIL(map_errno(err));
    uint32_t total = 0;
    for (int set = 0; set < 3; set++) {
        uint32_t va = GA(1 + set);
        if (!va) continue;
        uint32_t n = 0;
        for (int k = 0; k < count; k++) {
            if (!(want[k] & (1 << set))) continue;
            short re = fds[k].revents;
            int hit = set == 0 ? (re & (POLLIN | POLLHUP)) != 0
                    : set == 1 ? ((re & POLLOUT) && !(re & (POLLERR | POLLHUP)))
                               : (re & (POLLPRI | POLLERR)) != 0;
            if (hit) { memcpy(G(va + 4 + n * 4), &sock[k], 4); n++; }
        }
        memcpy(G(va), &n, 4);
        total += n;
    }
    *r = total;
    return GM_OK;
}

/* ---- names ---- */
static int ws_gethostname(const uint32_t *a, uint32_t *r) { /* (name, namelen): the machine name of the profile */
    static const char name[] = "BURNOUTPR";                 /* same as GetComputerNameA (runtime/guest_kernel_misc.c) */
    if (!GA(0) || GA(1) < sizeof name) FAIL(WSAEFAULT_);
    memcpy(G(GA(0)), name, sizeof name);
    *r = 0;
    return GM_OK;
}
static uint32_t resolve_error(int e) {
    switch (e) {
    case EAI_AGAIN: return WSATRY_AGAIN_;
    case EAI_FAIL: return WSANO_RECOVERY_;
    case EAI_MEMORY: return WSA_NOT_ENOUGH_MEMORY_;
    case EAI_FAMILY: return WSAEAFNOSUPPORT_;
    case EAI_SOCKTYPE: return WSAESOCKTNOSUPPORT_;
    case EAI_SERVICE: return 10109;                         /* WSATYPE_NOT_FOUND */
    default: return WSAHOST_NOT_FOUND_;
    }
}
/* Resolves through the host resolver; PORT_OFFLINE resolves only numeric
 * addresses and "localhost" (there is no name service without a network). */
static int host_lookup(const char *node, const char *service, int socktype, int protocol, int flags, struct addrinfo **out) {
    struct addrinfo hints; memset(&hints, 0, sizeof hints);
    hints.ai_family = AF_INET; hints.ai_socktype = socktype; hints.ai_protocol = protocol; hints.ai_flags = flags;
    if (offline() && node && strcmp(node, "localhost")) hints.ai_flags |= AI_NUMERICHOST;
    int held = st_machine_release();
    int rc = getaddrinfo(node, service, &hints, out);
    st_machine_reacquire(held);
    if (trace()) fprintf(stderr, "PORT_NET resolve('%s', '%s') -> %s\n", node ? node : "", service ? service : "", rc ? gai_strerror(rc) : "ok");
    return rc;
}
static RECOMP_TLS uint32_t t_hostent;                       /* per-thread hostent block in guest memory */
static int ws_gethostbyname(const uint32_t *a, uint32_t *r) {
    *r = 0;
    if (!GA(0)) { set_error(WSAEFAULT_); return GM_OK; }
    const char *name = (const char *)G(GA(0));
    char local[64];
    if (!name[0]) { snprintf(local, sizeof local, "localhost"); name = local; }
    struct addrinfo *list = NULL;
    int rc = host_lookup(name, NULL, SOCK_STREAM, 0, 0, &list);
    if (rc || !list) { set_error(resolve_error(rc)); return GM_OK; }
    /* hostent { char *h_name; char **h_aliases; short h_addrtype, h_length; char **h_addr_list; }
     * followed by its strings and tables, all in one guest block. */
    if (!t_hostent) t_hostent = d3d9t_guest_alloc(1024);
    if (!t_hostent) { freeaddrinfo(list); set_error(WSAENOBUFS_); return GM_OK; }
    uint32_t base = t_hostent, name_va = base + 16, aliases = base + 16 + 264, addr_list = aliases + 4, addrs = addr_list + 4 * 17;
    memset(G(base), 0, 1024);
    size_t len = strlen(name); if (len > 255) len = 255;
    memcpy(G(name_va), name, len);
    uint32_t count = 0;
    for (struct addrinfo *ai = list; ai && count < 16; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET) continue;
        uint32_t at = addrs + count * 4;
        memcpy(G(at), &((struct sockaddr_in *)ai->ai_addr)->sin_addr, 4);
        memcpy(G(addr_list + count * 4), &at, 4);
        count++;
    }
    freeaddrinfo(list);
    uint16_t type = 2, length = 4;
    memcpy(G(base), &name_va, 4); memcpy(G(base + 4), &aliases, 4); memcpy(G(base + 8), &type, 2); memcpy(G(base + 10), &length, 2);
    memcpy(G(base + 12), &addr_list, 4);
    if (!count) { set_error(WSANO_DATA_); return GM_OK; }
    *r = base;
    return GM_OK;
}
/* ADDRINFOA (32 bytes): flags, family, socktype, protocol, addrlen, canonname*, addr*, next*. */
static int ws_getaddrinfo(const uint32_t *a, uint32_t *r) { /* (node, service, hints, result) */
    if (!GA(3)) { *r = WSAEFAULT_; return GM_OK; }
    uint32_t zero = 0; memcpy(G(GA(3)), &zero, 4);
    int socktype = 0, protocol = 0, flags = 0;
    if (GA(2)) {
        int32_t h[4]; memcpy(h, G(GA(2)), 16);
        if (h[1] != 0 && h[1] != 2) { *r = WSAEAFNOSUPPORT_; set_error(*r); return GM_OK; }
        socktype = h[2] == 1 ? SOCK_STREAM : (h[2] == 2 ? SOCK_DGRAM : 0); protocol = h[3];
        if (h[0] & 1) flags |= AI_PASSIVE;
        if (h[0] & 4) flags |= AI_NUMERICHOST;
    }
    struct addrinfo *list = NULL;
    int rc = host_lookup(GA(0) ? (const char *)G(GA(0)) : NULL, GA(1) ? (const char *)G(GA(1)) : NULL, socktype, protocol, flags, &list);
    if (rc || !list) { *r = resolve_error(rc); set_error(*r); return GM_OK; }
    uint32_t head = 0, tail = 0;
    for (struct addrinfo *ai = list; ai; ai = ai->ai_next) {
        if (ai->ai_family != AF_INET) continue;
        uint32_t node = d3d9t_guest_alloc(48);
        if (!node) break;
        memset(G(node), 0, 48);
        uint32_t v[8] = { 0, 2, (uint32_t)(ai->ai_socktype == SOCK_STREAM ? 1 : (ai->ai_socktype == SOCK_DGRAM ? 2 : 0)),
                          (uint32_t)ai->ai_protocol, 16, 0, node + 32, 0 };
        memcpy(G(node), v, 32);
        uint8_t raw[16]; memset(raw, 0, sizeof raw);
        const struct sockaddr_in *sa = (const struct sockaddr_in *)ai->ai_addr;
        raw[0] = 2; memcpy(raw + 2, &sa->sin_port, 2); memcpy(raw + 4, &sa->sin_addr, 4);
        memcpy(G(node + 32), raw, 16);
        if (tail) memcpy(G(tail + 28), &node, 4); else head = node;
        tail = node;
    }
    freeaddrinfo(list);
    if (!head) { *r = WSANO_DATA_; set_error(*r); return GM_OK; }
    memcpy(G(GA(3)), &head, 4);
    *r = 0;
    return GM_OK;
}
static int ws_freeaddrinfo(const uint32_t *a, uint32_t *r) {
    (void)r;
    uint32_t node = GA(0);
    while (node) { uint32_t next; memcpy(&next, G(node + 28), 4); d3d9t_guest_free(node); node = next; }
    return GM_OK;
}

/* ---- events: manual-reset events of the sync-object model ---- */
static int ws_create_event(const uint32_t *a, uint32_t *r) {
    (void)a;
    uint32_t handle = 0;
    int rc = gso_create_event(g_mem, g_teb, 0, 1, 0, 0, &handle);
    if (rc) return rc;
    *r = handle;                                            /* 0 = WSA_INVALID_EVENT */
    return GM_OK;
}
static int ws_set_event(const uint32_t *a, uint32_t *r) { return gso_set_event(g_mem, g_teb, GA(0), r); }
static int ws_reset_event(const uint32_t *a, uint32_t *r) { return gso_reset_event(g_mem, g_teb, GA(0), r); }
static int ws_close_event(const uint32_t *a, uint32_t *r) { return sb_close_handle_public(GA(0), r); }
static int ws_wait_for_multiple_events(const uint32_t *a, uint32_t *r) {   /* (count, handles, waitAll, timeout, alertable) */
    int held = st_machine_release();
    uint32_t value = 0;
    int rc = gso_wait_multiple_ex(g_mem, g_teb, GA(1), GA(0), GA(2), GA(3), GA(4), &value);
    st_machine_reacquire(held);
    *r = value;                                             /* WSA_WAIT_* have the WAIT_* values */
    return rc;
}

const OrigBridge orig_winsock_bridges[] = {
    { "ws2_32.dll", "htons", 4, ws_htons }, { "ws2_32.dll", "ntohs", 4, ws_ntohs },
    { "ws2_32.dll", "htonl", 4, ws_htonl }, { "ws2_32.dll", "ntohl", 4, ws_ntohl },
    { "ws2_32.dll", "WSAGetLastError", 0, ws_get_last_error },
    { "ws2_32.dll", "socket", 12, ws_socket }, { "ws2_32.dll", "closesocket", 4, ws_closesocket },
    { "ws2_32.dll", "shutdown", 8, ws_shutdown }, { "ws2_32.dll", "bind", 12, ws_bind },
    { "ws2_32.dll", "connect", 12, ws_connect }, { "ws2_32.dll", "accept", 12, ws_accept },
    { "ws2_32.dll", "getsockname", 12, ws_getsockname }, { "ws2_32.dll", "getpeername", 12, ws_getpeername },
    { "ws2_32.dll", "send", 16, ws_send }, { "ws2_32.dll", "recv", 16, ws_recv },
    { "ws2_32.dll", "sendto", 24, ws_sendto }, { "ws2_32.dll", "recvfrom", 24, ws_recvfrom },
    { "ws2_32.dll", "WSARecv", 28, ws_wsa_recv }, { "ws2_32.dll", "WSARecvFrom", 36, ws_wsa_recv_from },
    { "ws2_32.dll", "WSAGetOverlappedResult", 20, ws_wsa_get_overlapped_result },
    { "ws2_32.dll", "setsockopt", 20, ws_setsockopt }, { "ws2_32.dll", "getsockopt", 20, ws_getsockopt },
    { "ws2_32.dll", "ioctlsocket", 12, ws_ioctlsocket }, { "ws2_32.dll", "WSAIoctl", 36, ws_wsa_ioctl },
    { "ws2_32.dll", "select", 20, ws_select },
    { "ws2_32.dll", "gethostname", 8, ws_gethostname }, { "ws2_32.dll", "gethostbyname", 4, ws_gethostbyname },
    { "ws2_32.dll", "getaddrinfo", 16, ws_getaddrinfo }, { "ws2_32.dll", "freeaddrinfo", 4, ws_freeaddrinfo },
    { "ws2_32.dll", "WSACreateEvent", 0, ws_create_event }, { "ws2_32.dll", "WSASetEvent", 4, ws_set_event },
    { "ws2_32.dll", "WSAResetEvent", 4, ws_reset_event }, { "ws2_32.dll", "WSACloseEvent", 4, ws_close_event },
    { "ws2_32.dll", "WSAWaitForMultipleEvents", 20, ws_wait_for_multiple_events },
};
const uint32_t orig_winsock_bridge_count = sizeof orig_winsock_bridges / sizeof orig_winsock_bridges[0];
