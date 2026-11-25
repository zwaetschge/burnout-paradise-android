#ifndef BURNOUT_GUEST_WINSOCK_NET_H
#define BURNOUT_GUEST_WINSOCK_NET_H
#include <stdint.h>
#include "guest_memory.h"
#include "guest_bridge_abi.h"

/* Real host-socket bridges for ws2_32.
 *
 * Unlike runtime/guest_winsock.c (which only models WSAStartup/WSACleanup and
 * never opens a connection), this module performs the real BSD socket call and
 * reports the real result. Nothing is faked: connect() returns the host
 * outcome, recv() returns the bytes the peer actually sent, and a refused or
 * unreachable endpoint produces the documented WSA error.
 *
 * Modelled:
 *   - socket: AF_INET only, SOCK_STREAM / SOCK_DGRAM, protocol 0/TCP/UDP.
 *     The host socket is created with SOCK_CLOEXEC.
 *   - connect: a PE32 sockaddr_in (16 bytes) is read and passed through.
 *     Blocking, exactly like a Winsock blocking socket.
 *   - send / recv / shutdown / closesocket: real calls, real byte counts.
 *   - htons, inet_pton (AF_INET dotted quad).
 *   - WSAGetLastError: the per-thread Winsock error, mirrored into the guest
 *     TEB LastError field (+0x34) so both the API and GetLastError agree.
 *   - errno -> WSA error mapping for the documented codes.
 *
 * Documented limits: no AF_INET6, no SOCK_RAW, no bind/listen/accept, no
 * non-blocking/overlapped sockets, no IOCP, no TCP_NODELAY setsockopt, no
 * WSACleanup teardown of live sockets (that stays in the WSAStartup model).
 * SOCKET values are runtime-issued handles from runtime/guest_handles.h; no
 * host file descriptor is ever visible to the guest.
 *
 * Guest memory is only touched through gm_check/gm_read/gm_write. A non-zero
 * function return is a named stop, never a Win32 status. */

#define GWN_INVALID_SOCKET 0xffffffffu
#define GWN_SOCKET_ERROR 0xffffffffu

#define GWN_AF_INET 2u
#define GWN_SOCK_STREAM 1u
#define GWN_SOCK_DGRAM 2u
#define GWN_SOCK_RAW 3u
#define GWN_IPPROTO_TCP 6u
#define GWN_IPPROTO_UDP 17u
#define GWN_SOCKADDR_IN_SIZE 16u

/* Winsock error codes (WSAE*) used by the mapping. */
#define GWN_WSABASEERR 10000u
#define GWN_WSAEINTR 10004u
#define GWN_WSAEBADF 10009u
#define GWN_WSAEACCES 10013u
#define GWN_WSAEFAULT 10014u
#define GWN_WSAEINVAL 10022u
#define GWN_WSAEMFILE 10024u
#define GWN_WSAEWOULDBLOCK 10035u
#define GWN_WSAEINPROGRESS 10036u
#define GWN_WSAEALREADY 10037u
#define GWN_WSAENOTSOCK 10038u
#define GWN_WSAEDESTADDRREQ 10039u
#define GWN_WSAEMSGSIZE 10040u
#define GWN_WSAEPROTOTYPE 10041u
#define GWN_WSAENOPROTOOPT 10042u
#define GWN_WSAEPROTONOSUPPORT 10043u
#define GWN_WSAESOCKTNOSUPPORT 10044u
#define GWN_WSAEOPNOTSUPP 10045u
#define GWN_WSAEAFNOSUPPORT 10047u
#define GWN_WSAEADDRINUSE 10048u
#define GWN_WSAEADDRNOTAVAIL 10049u
#define GWN_WSAENETDOWN 10050u
#define GWN_WSAENETUNREACH 10051u
#define GWN_WSAENETRESET 10052u
#define GWN_WSAECONNABORTED 10053u
#define GWN_WSAECONNRESET 10054u
#define GWN_WSAENOBUFS 10055u
#define GWN_WSAEISCONN 10056u
#define GWN_WSAENOTCONN 10057u
#define GWN_WSAESHUTDOWN 10058u
#define GWN_WSAETIMEDOUT 10060u
#define GWN_WSAECONNREFUSED 10061u
#define GWN_WSAEHOSTUNREACH 10065u

/* Module-local named stops. */
enum {
    GU_STOP_GWN_BAD_ARGS = -2501,
    GU_STOP_GWN_BUFFER = -2502,   /* guest buffer does not fit the model */
    GU_STOP_GWN_OVERLAPPED = -2503,
    GU_STOP_GWN_HOST = -2504      /* host call failed outside errno mapping */
};

typedef struct GuestWinsockNet GuestWinsockNet;

int gwn_create(GuestMemory *, uint32_t teb, GuestWinsockNet **out);
void gwn_destroy(GuestWinsockNet *);
/* Live sockets owned by this module (diagnostics). */
uint32_t gwn_live_sockets(const GuestWinsockNet *);
uint32_t gwn_last_error(const GuestWinsockNet *);
uint32_t gwn_connects(const GuestWinsockNet *);
uint32_t gwn_bytes_sent(const GuestWinsockNet *);
uint32_t gwn_bytes_received(const GuestWinsockNet *);
const char *gwn_last_endpoint(const GuestWinsockNet *);
const char *gwn_last_stop(const GuestWinsockNet *);
const char *gwn_stop_name(int code);
/* Closes every socket this module issued (host teardown only). */
void gwn_close_all(GuestWinsockNet *);

int gwn_socket(GuestWinsockNet *, const uint32_t *args, uint32_t *result);
int gwn_connect(GuestWinsockNet *, const uint32_t *args, uint32_t *result);
int gwn_send(GuestWinsockNet *, const uint32_t *args, uint32_t *result);
int gwn_recv(GuestWinsockNet *, const uint32_t *args, uint32_t *result);
int gwn_shutdown(GuestWinsockNet *, const uint32_t *args, uint32_t *result);
int gwn_closesocket(GuestWinsockNet *, const uint32_t *args, uint32_t *result);
int gwn_htons(GuestWinsockNet *, const uint32_t *args, uint32_t *result);
int gwn_inet_pton(GuestWinsockNet *, const uint32_t *args, uint32_t *result);
int gwn_get_last_error(GuestWinsockNet *, const uint32_t *args,
                       uint32_t *result);

typedef struct {
    const char *dll;
    const char *name;
    uint32_t stdcall_arg_bytes;
    int (*fn)(GuestWinsockNet *, const uint32_t *args, uint32_t *result);
} GuestWinsockNetExport;

extern const GuestWinsockNetExport ws2_32_net_exports[];
extern const uint32_t ws2_32_net_export_count;

#endif
