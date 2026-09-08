#ifndef BSD_H
#define BSD_H

// top-most wrapper of bsd-like syscalls

// holds everything you need from the bsd/winsock interfaces, only included by internal libusockets.h
// here everything about the syscalls are inline-wrapped and included

#include "libusockets.h"

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#pragma comment(lib, "ws2_32.lib")
#define SETSOCKOPT_PTR_TYPE const char *
#define LIBUS_SOCKET_ERROR INVALID_SOCKET
#else
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
/* For socklen_t */
#include <sys/socket.h>
#define SETSOCKOPT_PTR_TYPE int *
#define LIBUS_SOCKET_ERROR -1
#endif

struct bsd_addr_t {
    struct sockaddr_storage mem;
    socklen_t len;
    char *ip;
    int ip_length;
    int port;
};

LIBUS_SOCKET_DESCRIPTOR apple_no_sigpipe(LIBUS_SOCKET_DESCRIPTOR fd);
LIBUS_SOCKET_DESCRIPTOR bsd_set_nonblocking(LIBUS_SOCKET_DESCRIPTOR fd);
void bsd_socket_nodelay(LIBUS_SOCKET_DESCRIPTOR fd, int enabled);
void bsd_socket_flush(LIBUS_SOCKET_DESCRIPTOR fd);
LIBUS_SOCKET_DESCRIPTOR bsd_create_socket(int domain, int type, int protocol);

void bsd_close_socket(LIBUS_SOCKET_DESCRIPTOR fd);
void bsd_shutdown_socket(LIBUS_SOCKET_DESCRIPTOR fd);
void bsd_shutdown_socket_read(LIBUS_SOCKET_DESCRIPTOR fd);

void internal_finalize_bsd_addr(struct bsd_addr_t *addr);

int bsd_local_addr(LIBUS_SOCKET_DESCRIPTOR fd, struct bsd_addr_t *addr);
int bsd_remote_addr(LIBUS_SOCKET_DESCRIPTOR fd, struct bsd_addr_t *addr);

char *bsd_addr_get_ip(struct bsd_addr_t *addr);
int bsd_addr_get_ip_length(struct bsd_addr_t *addr);

int bsd_addr_get_port(struct bsd_addr_t *addr);

// called by dispatch_ready_poll
LIBUS_SOCKET_DESCRIPTOR bsd_accept_socket(LIBUS_SOCKET_DESCRIPTOR fd, struct bsd_addr_t *addr);

int bsd_recv(LIBUS_SOCKET_DESCRIPTOR fd, void *buf, int length, int flags);
int bsd_send(LIBUS_SOCKET_DESCRIPTOR fd, const char *buf, int length, int msg_more);
int bsd_write2(LIBUS_SOCKET_DESCRIPTOR fd, const char *header, int header_length, const char *payload, int payload_length);
int bsd_would_block();

/* Map a raw socket error to a POSIX errno name space. On Windows getsockopt(SO_ERROR) and the connect
 * paths yield WSAE* codes (10061, 10060, ...) that never equal the POSIX E* values the µWS layer compares
 * against, so refused/timed-out connects would otherwise all surface as ECONNRESET. A no-op elsewhere. */
int bsd_normalize_socket_error(int error);

/* Last socket error for the calling thread: WSAGetLastError() on Windows, errno elsewhere. errno is not
 * set by failed Winsock socket()/bind() calls, so read this instead of errno at those sites. */
int bsd_socket_last_error();

// return LIBUS_SOCKET_ERROR or the fd that represents listen socket
// listen both on ipv6 and ipv4
LIBUS_SOCKET_DESCRIPTOR bsd_create_listen_socket(const char *host, int port, int options);

LIBUS_SOCKET_DESCRIPTOR bsd_create_listen_socket_unix(const char *path, int options);

/* Create a non-blocking connect socket to an already-resolved address. Optionally binds to
 * source_host first. Returns the fd on success or when connect() is in progress (EINPROGRESS),
 * LIBUS_SOCKET_ERROR otherwise. */
LIBUS_SOCKET_DESCRIPTOR bsd_create_connect_socket_addr(const struct sockaddr *addr, socklen_t addrlen, const char *source_host);

#endif // BSD_H
