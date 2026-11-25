/* Real host-socket bridges for ws2_32. See guest_winsock_net.h.
 *
 * Every call goes to the host BSD socket layer; the result is the host result.
 * No connection is simulated, no packet is fabricated and no endpoint is
 * pre-resolved. Guest memory is only touched through gm_check/gm_read/gm_write
 * and the host file descriptor stays inside the handle object. */
#include "guest_winsock_net.h"
#include "guest_handles.h"
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

typedef struct {
    int fd;
    uint32_t magic;
} GwnSocket;

#define GWN_SOCKET_MAGIC 0x4e574747u /* "GWWN" */

struct GuestWinsockNet {
    GuestMemory *memory;
    uint32_t teb;
    uint32_t last_error;
    uint32_t live_sockets;
    uint32_t connects;
    uint32_t bytes_sent;
    uint32_t bytes_received;
    char last_endpoint[64];
    char last_stop[160];
};

static void put32(uint8_t *b, uint32_t v) {
    b[0] = (uint8_t)v; b[1] = (uint8_t)(v >> 8);
    b[2] = (uint8_t)(v >> 16); b[3] = (uint8_t)(v >> 24);
}
static int write_u32(GuestWinsockNet *net, uint32_t address, uint32_t value) {
    uint8_t b[4];
    put32(b, value);
    return gm_write(net->memory, address, b, 4);
}
/* Winsock errors are per thread: keep the module state and the guest TEB in
 * sync so WSAGetLastError and GetLastError agree. */
static int set_error(GuestWinsockNet *net, uint32_t code) {
    net->last_error = code;
    return write_u32(net, net->teb + 0x34, code);
}
static int stop(GuestWinsockNet *net, int code, const char *what) {
    snprintf(net->last_stop, sizeof(net->last_stop), "%s", what);
    fprintf(stderr, "winsock-net-stop: %s (%d)\n", what, code);
    return code;
}
static uint32_t map_errno(int error) {
    switch (error) {
    case 0: return 0;
    case EINTR: return GWN_WSAEINTR;
    case EBADF: return GWN_WSAEBADF;
    case EACCES:
    case EPERM: return GWN_WSAEACCES;
    case EFAULT: return GWN_WSAEFAULT;
    case EINVAL: return GWN_WSAEINVAL;
    case EMFILE:
    case ENFILE: return GWN_WSAEMFILE;
    case EAGAIN: /* == EWOULDBLOCK on Linux/Android */
        return GWN_WSAEWOULDBLOCK;
    case EINPROGRESS: return GWN_WSAEINPROGRESS;
    case EALREADY: return GWN_WSAEALREADY;
    case ENOTSOCK: return GWN_WSAENOTSOCK;
    case EDESTADDRREQ: return GWN_WSAEDESTADDRREQ;
    case EMSGSIZE: return GWN_WSAEMSGSIZE;
    case EPROTOTYPE: return GWN_WSAEPROTOTYPE;
    case ENOPROTOOPT: return GWN_WSAENOPROTOOPT;
    case EPROTONOSUPPORT: return GWN_WSAEPROTONOSUPPORT;
    case ESOCKTNOSUPPORT: return GWN_WSAESOCKTNOSUPPORT;
    case EOPNOTSUPP: return GWN_WSAEOPNOTSUPP;
    case EAFNOSUPPORT: return GWN_WSAEAFNOSUPPORT;
    case EADDRINUSE: return GWN_WSAEADDRINUSE;
    case EADDRNOTAVAIL: return GWN_WSAEADDRNOTAVAIL;
    case ENETDOWN: return GWN_WSAENETDOWN;
    case ENETUNREACH: return GWN_WSAENETUNREACH;
    case ENETRESET: return GWN_WSAENETRESET;
    case ECONNABORTED: return GWN_WSAECONNABORTED;
    case ECONNRESET:
    case EPIPE: return GWN_WSAECONNRESET;
    case ENOBUFS:
    case ENOMEM: return GWN_WSAENOBUFS;
    case EISCONN: return GWN_WSAEISCONN;
    case ENOTCONN: return GWN_WSAENOTCONN;
    case ESHUTDOWN: return GWN_WSAESHUTDOWN;
    case ETIMEDOUT: return GWN_WSAETIMEDOUT;
    case ECONNREFUSED: return GWN_WSAECONNREFUSED;
    case EHOSTUNREACH: return GWN_WSAEHOSTUNREACH;
    default: return GWN_WSAEINVAL;
    }
}

int gwn_create(GuestMemory *memory, uint32_t teb, GuestWinsockNet **out) {
    if (!memory || !out) return GM_RANGE;
    GuestWinsockNet *net = (GuestWinsockNet *)calloc(1, sizeof(*net));
    if (!net) return GM_ALLOCATION;
    net->memory = memory;
    net->teb = teb;
    *out = net;
    return GM_OK;
}
static void socket_destroy(void *object) {
    GwnSocket *socket_object = (GwnSocket *)object;
    if (!socket_object) return;
    if (socket_object->fd >= 0) (void)close(socket_object->fd);
    free(socket_object);
}
void gwn_destroy(GuestWinsockNet *net) {
    if (!net) return;
    gwn_close_all(net);
    free(net);
}
void gwn_close_all(GuestWinsockNet *net) {
    if (!net) return;
    for (uint32_t i = 0; i < GH_MAX_HANDLES; i++) {
        GwnSocket *socket_object = (GwnSocket *)gh_lookup(
            GH_FIRST_HANDLE + (i << 2), GH_KIND_OTHER, NULL);
        if (socket_object && socket_object->magic == GWN_SOCKET_MAGIC) {
            (void)gh_close(GH_FIRST_HANDLE + (i << 2));
            if (net->live_sockets) net->live_sockets--;
        }
    }
}
uint32_t gwn_live_sockets(const GuestWinsockNet *net) {
    return net ? net->live_sockets : 0u;
}
uint32_t gwn_last_error(const GuestWinsockNet *net) {
    return net ? net->last_error : 0u;
}
uint32_t gwn_connects(const GuestWinsockNet *net) {
    return net ? net->connects : 0u;
}
uint32_t gwn_bytes_sent(const GuestWinsockNet *net) {
    return net ? net->bytes_sent : 0u;
}
uint32_t gwn_bytes_received(const GuestWinsockNet *net) {
    return net ? net->bytes_received : 0u;
}
const char *gwn_last_endpoint(const GuestWinsockNet *net) {
    return (net && net->last_endpoint[0]) ? net->last_endpoint : NULL;
}
const char *gwn_last_stop(const GuestWinsockNet *net) {
    return (net && net->last_stop[0]) ? net->last_stop : NULL;
}
const char *gwn_stop_name(int code) {
    switch (code) {
    case 0: return "ok";
    case 1: return "gm-range";
    case 2: return "gm-uncommitted";
    case 3: return "gm-allocation";
    case 4: return "gm-access";
    case 5: return "gm-unsupported";
    case 6: return "gm-overlap";
    case GU_STOP_GWN_BAD_ARGS: return "winsock-argument-outside-model";
    case GU_STOP_GWN_BUFFER: return "winsock-guest-buffer-too-large";
    case GU_STOP_GWN_OVERLAPPED: return "winsock-overlapped-unsupported";
    case GU_STOP_GWN_HOST: return "winsock-host-call-failed";
    default: return "unknown-stop";
    }
}

static GwnSocket *socket_lookup(uint32_t handle) {
    GwnSocket *socket_object = (GwnSocket *)gh_lookup(handle, GH_KIND_OTHER, NULL);
    if (!socket_object || socket_object->magic != GWN_SOCKET_MAGIC) return NULL;
    return socket_object;
}

int gwn_socket(GuestWinsockNet *net, const uint32_t *args, uint32_t *result) {
    if (!net || !args || !result) return GU_STOP_GWN_BAD_ARGS;
    uint32_t af = args[0], type = args[1], protocol = args[2];
    if (af != GWN_AF_INET) {
        int rc = set_error(net, GWN_WSAEAFNOSUPPORT);
        if (rc) return rc;
        *result = GWN_INVALID_SOCKET;
        return GM_OK;
    }
    if (type != GWN_SOCK_STREAM && type != GWN_SOCK_DGRAM) {
        int rc = set_error(net, GWN_WSAESOCKTNOSUPPORT);
        if (rc) return rc;
        *result = GWN_INVALID_SOCKET;
        return GM_OK;
    }
    if (protocol != 0 &&
        !(type == GWN_SOCK_STREAM && protocol == GWN_IPPROTO_TCP) &&
        !(type == GWN_SOCK_DGRAM && protocol == GWN_IPPROTO_UDP)) {
        int rc = set_error(net, GWN_WSAEPROTONOSUPPORT);
        if (rc) return rc;
        *result = GWN_INVALID_SOCKET;
        return GM_OK;
    }
    int fd = socket(AF_INET,
                    (type == GWN_SOCK_STREAM ? SOCK_STREAM : SOCK_DGRAM) |
                        SOCK_CLOEXEC,
                    protocol);
    if (fd < 0) {
        int rc = set_error(net, map_errno(errno));
        if (rc) return rc;
        *result = GWN_INVALID_SOCKET;
        return GM_OK;
    }
    GwnSocket *socket_object = (GwnSocket *)calloc(1, sizeof(*socket_object));
    if (!socket_object) {
        (void)close(fd);
        return GM_ALLOCATION;
    }
    socket_object->fd = fd;
    socket_object->magic = GWN_SOCKET_MAGIC;
    uint32_t handle = gh_insert(GH_KIND_OTHER, socket_object, socket_destroy);
    if (!handle) {
        (void)close(fd);
        free(socket_object);
        return GM_ALLOCATION;
    }
    net->live_sockets++;
    *result = handle;
    return GM_OK;
}
int gwn_connect(GuestWinsockNet *net, const uint32_t *args, uint32_t *result) {
    if (!net || !args || !result) return GU_STOP_GWN_BAD_ARGS;
    GwnSocket *socket_object = socket_lookup(args[0]);
    if (!socket_object) {
        int rc = set_error(net, GWN_WSAENOTSOCK);
        if (rc) return rc;
        *result = GWN_SOCKET_ERROR;
        return GM_OK;
    }
    uint32_t length = args[2];
    if (length != GWN_SOCKADDR_IN_SIZE)
        return stop(net, GU_STOP_GWN_BAD_ARGS,
                    "connect: only a 16-byte sockaddr_in is modelled");
    uint8_t name[GWN_SOCKADDR_IN_SIZE];
    int rc = gm_read(net->memory, args[1], name, sizeof(name));
    if (rc) return rc;
    if (((uint32_t)name[0] | ((uint32_t)name[1] << 8)) != GWN_AF_INET) {
        rc = set_error(net, GWN_WSAEAFNOSUPPORT);
        if (rc) return rc;
        *result = GWN_SOCKET_ERROR;
        return GM_OK;
    }
    struct sockaddr_in address;
    memset(&address, 0, sizeof(address));
    address.sin_family = AF_INET;
    memcpy(&address.sin_port, name + 2, 2);
    memcpy(&address.sin_addr, name + 4, 4);
    snprintf(net->last_endpoint, sizeof(net->last_endpoint), "%u.%u.%u.%u:%u",
             name[4], name[5], name[6], name[7],
             (unsigned)(((uint32_t)name[2] << 8) | name[3]));
    net->connects++;
    if (connect(socket_object->fd, (struct sockaddr *)&address,
                sizeof(address)) != 0) {
        rc = set_error(net, map_errno(errno));
        if (rc) return rc;
        *result = GWN_SOCKET_ERROR;
        return GM_OK;
    }
    rc = set_error(net, 0);
    if (rc) return rc;
    *result = 0;
    return GM_OK;
}
int gwn_send(GuestWinsockNet *net, const uint32_t *args, uint32_t *result) {
    if (!net || !args || !result) return GU_STOP_GWN_BAD_ARGS;
    GwnSocket *socket_object = socket_lookup(args[0]);
    if (!socket_object) {
        int rc = set_error(net, GWN_WSAENOTSOCK);
        if (rc) return rc;
        *result = GWN_SOCKET_ERROR;
        return GM_OK;
    }
    if (args[3]) return stop(net, GU_STOP_GWN_BAD_ARGS,
                             "send: only flags 0 is modelled");
    uint32_t length = args[2];
    if (length > 1024u * 1024u)
        return stop(net, GU_STOP_GWN_BUFFER,
                    "send: guest buffer larger than 1 MiB");
    int rc = gm_check(net->memory, args[1], length, GM_READ);
    if (rc) return rc;
    uint8_t chunk[4096];
    uint32_t done = 0;
    while (done < length) {
        uint32_t step = length - done;
        if (step > sizeof(chunk)) step = sizeof(chunk);
        rc = gm_read(net->memory, args[1] + done, chunk, step);
        if (rc) return rc;
        ssize_t sent = send(socket_object->fd, chunk, step, 0);
        if (sent < 0) {
            rc = set_error(net, map_errno(errno));
            if (rc) return rc;
            *result = GWN_SOCKET_ERROR;
            return GM_OK;
        }
        done += (uint32_t)sent;
        net->bytes_sent += (uint32_t)sent;
        if ((uint32_t)sent < step) break; /* short write: report what happened */
    }
    rc = set_error(net, 0);
    if (rc) return rc;
    *result = done;
    return GM_OK;
}
int gwn_recv(GuestWinsockNet *net, const uint32_t *args, uint32_t *result) {
    if (!net || !args || !result) return GU_STOP_GWN_BAD_ARGS;
    GwnSocket *socket_object = socket_lookup(args[0]);
    if (!socket_object) {
        int rc = set_error(net, GWN_WSAENOTSOCK);
        if (rc) return rc;
        *result = GWN_SOCKET_ERROR;
        return GM_OK;
    }
    if (args[3]) return stop(net, GU_STOP_GWN_BAD_ARGS,
                             "recv: only flags 0 is modelled");
    uint32_t length = args[2];
    if (length > 1024u * 1024u)
        return stop(net, GU_STOP_GWN_BUFFER,
                    "recv: guest buffer larger than 1 MiB");
    int rc = gm_check(net->memory, args[1], length, GM_WRITE);
    if (rc) return rc;
    uint8_t chunk[4096];
    uint32_t step = length < sizeof(chunk) ? length : (uint32_t)sizeof(chunk);
    if (!step) { *result = 0; return GM_OK; }
    ssize_t received = recv(socket_object->fd, chunk, step, 0);
    if (received < 0) {
        rc = set_error(net, map_errno(errno));
        if (rc) return rc;
        *result = GWN_SOCKET_ERROR;
        return GM_OK;
    }
    if (received) {
        rc = gm_write(net->memory, args[1], chunk, (uint32_t)received);
        if (rc) return rc;
        net->bytes_received += (uint32_t)received;
    }
    rc = set_error(net, 0);
    if (rc) return rc;
    *result = (uint32_t)received;
    return GM_OK;
}
int gwn_shutdown(GuestWinsockNet *net, const uint32_t *args, uint32_t *result) {
    if (!net || !args || !result) return GU_STOP_GWN_BAD_ARGS;
    GwnSocket *socket_object = socket_lookup(args[0]);
    if (!socket_object) {
        int rc = set_error(net, GWN_WSAENOTSOCK);
        if (rc) return rc;
        *result = GWN_SOCKET_ERROR;
        return GM_OK;
    }
    int how;
    switch (args[1]) {
    case 0: how = SHUT_RD; break;
    case 1: how = SHUT_WR; break;
    case 2: how = SHUT_RDWR; break;
    default:
        return stop(net, GU_STOP_GWN_BAD_ARGS,
                    "shutdown: how must be SD_RECEIVE/SD_SEND/SD_BOTH");
    }
    if (shutdown(socket_object->fd, how) != 0) {
        int rc = set_error(net, map_errno(errno));
        if (rc) return rc;
        *result = GWN_SOCKET_ERROR;
        return GM_OK;
    }
    int rc = set_error(net, 0);
    if (rc) return rc;
    *result = 0;
    return GM_OK;
}
int gwn_closesocket(GuestWinsockNet *net, const uint32_t *args,
                    uint32_t *result) {
    if (!net || !args || !result) return GU_STOP_GWN_BAD_ARGS;
    GwnSocket *socket_object = socket_lookup(args[0]);
    if (!socket_object) {
        int rc = set_error(net, GWN_WSAENOTSOCK);
        if (rc) return rc;
        *result = GWN_SOCKET_ERROR;
        return GM_OK;
    }
    if (!gh_close(args[0])) return stop(net, GU_STOP_GWN_HOST,
                                        "closesocket: handle table refused");
    if (net->live_sockets) net->live_sockets--;
    int rc = set_error(net, 0);
    if (rc) return rc;
    *result = 0;
    return GM_OK;
}
int gwn_htons(GuestWinsockNet *net, const uint32_t *args, uint32_t *result) {
    if (!net || !args || !result) return GU_STOP_GWN_BAD_ARGS;
    uint32_t value = args[0] & 0xffffu;
    *result = ((value & 0xffu) << 8) | ((value >> 8) & 0xffu);
    return GM_OK;
}
int gwn_inet_pton(GuestWinsockNet *net, const uint32_t *args,
                  uint32_t *result) {
    if (!net || !args || !result) return GU_STOP_GWN_BAD_ARGS;
    if (args[0] != GWN_AF_INET) {
        int rc = set_error(net, GWN_WSAEAFNOSUPPORT);
        if (rc) return rc;
        *result = 0xffffffffu;
        return GM_OK;
    }
    if (!args[2]) return stop(net, GU_STOP_GWN_BAD_ARGS,
                              "inet_pton: dst must not be NULL");
    char text[64];
    uint32_t length = 0;
    for (; length < sizeof(text) - 1; length++) {
        uint8_t byte;
        int rc = gm_read(net->memory, args[1] + length, &byte, 1);
        if (rc) return rc;
        if (!byte) break;
        text[length] = (char)byte;
    }
    text[length] = 0;
    uint8_t address[4];
    uint32_t octets = 0, value = 0, digits = 0;
    for (uint32_t i = 0; i <= length; i++) {
        char c = text[i];
        if (c >= '0' && c <= '9') {
            value = value * 10u + (uint32_t)(c - '0');
            if (value > 255) { octets = 0; break; }
            digits++;
            continue;
        }
        if (c == '.' || c == 0) {
            if (!digits || octets >= 4) { octets = 0; break; }
            address[octets++] = (uint8_t)value;
            value = 0;
            digits = 0;
            if (c == 0) break;
            continue;
        }
        octets = 0;
        break;
    }
    if (octets != 4) {
        int rc = set_error(net, GWN_WSAEINVAL);
        if (rc) return rc;
        *result = 0; /* documented: not a valid address */
        return GM_OK;
    }
    int rc = gm_write(net->memory, args[2], address, 4);
    if (rc) return rc;
    rc = set_error(net, 0);
    if (rc) return rc;
    *result = 1;
    return GM_OK;
}
int gwn_get_last_error(GuestWinsockNet *net, const uint32_t *args,
                       uint32_t *result) {
    (void)args;
    if (!net || !result) return GU_STOP_GWN_BAD_ARGS;
    *result = net->last_error;
    return GM_OK;
}

/* --- registration list -------------------------------------------------- */
#define GWN_ARGS(n) ((n) * 4u)
const GuestWinsockNetExport ws2_32_net_exports[] = {
    { "ws2_32.dll", "socket", GWN_ARGS(3), gwn_socket },
    { "ws2_32.dll", "connect", GWN_ARGS(3), gwn_connect },
    { "ws2_32.dll", "send", GWN_ARGS(4), gwn_send },
    { "ws2_32.dll", "recv", GWN_ARGS(4), gwn_recv },
    { "ws2_32.dll", "shutdown", GWN_ARGS(2), gwn_shutdown },
    { "ws2_32.dll", "closesocket", GWN_ARGS(1), gwn_closesocket },
    { "ws2_32.dll", "htons", GWN_ARGS(1), gwn_htons },
    { "ws2_32.dll", "inet_pton", GWN_ARGS(3), gwn_inet_pton },
    { "ws2_32.dll", "WSAGetLastError", GWN_ARGS(0), gwn_get_last_error }
};
const uint32_t ws2_32_net_export_count =
    (uint32_t)(sizeof(ws2_32_net_exports) / sizeof(ws2_32_net_exports[0]));
