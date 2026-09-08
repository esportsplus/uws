/* Todo: this file should lie in networking/bsd.c */

#include "libusockets.h"
#include "internal/internal.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <mstcpip.h>
#endif

#ifndef _WIN32
//#define _GNU_SOURCE
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <netdb.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#endif

LIBUS_SOCKET_DESCRIPTOR apple_no_sigpipe(LIBUS_SOCKET_DESCRIPTOR fd) {
#ifdef __APPLE__
    if (fd != LIBUS_SOCKET_ERROR) {
        int no_sigpipe = 1;
        setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, (void *) &no_sigpipe, sizeof(int));
    }
#endif
    return fd;
}

LIBUS_SOCKET_DESCRIPTOR bsd_set_nonblocking(LIBUS_SOCKET_DESCRIPTOR fd) {
#ifdef _WIN32
    /* Libuv will set windows sockets as non-blocking */
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
    return fd;
}

void bsd_socket_nodelay(LIBUS_SOCKET_DESCRIPTOR fd, int enabled) {
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (void *) &enabled, sizeof(enabled));
}

void bsd_socket_flush(LIBUS_SOCKET_DESCRIPTOR fd) {
    // Linux TCP_CORK has the same underlying corking mechanism as with MSG_MORE
#ifdef TCP_CORK
    int enabled = 0;
    setsockopt(fd, IPPROTO_TCP, TCP_CORK, (void *) &enabled, sizeof(int));
#endif
}

LIBUS_SOCKET_DESCRIPTOR bsd_create_socket(int domain, int type, int protocol) {
    // returns INVALID_SOCKET on error
    int flags = 0;
#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
    flags = SOCK_CLOEXEC | SOCK_NONBLOCK;
#endif

    LIBUS_SOCKET_DESCRIPTOR created_fd = socket(domain, type | flags, protocol);

    return bsd_set_nonblocking(apple_no_sigpipe(created_fd));
}

void bsd_close_socket(LIBUS_SOCKET_DESCRIPTOR fd) {
#ifdef _WIN32
    closesocket(fd);
#else
    close(fd);
#endif
}

void bsd_shutdown_socket(LIBUS_SOCKET_DESCRIPTOR fd) {
#ifdef _WIN32
    shutdown(fd, SD_SEND);
#else
    shutdown(fd, SHUT_WR);
#endif
}

void bsd_shutdown_socket_read(LIBUS_SOCKET_DESCRIPTOR fd) {
#ifdef _WIN32
    shutdown(fd, SD_RECEIVE);
#else
    shutdown(fd, SHUT_RD);
#endif
}

void internal_finalize_bsd_addr(struct bsd_addr_t *addr) {
    // parse, so to speak, the address
    if (addr->mem.ss_family == AF_INET6) {
        addr->ip = (char *) &((struct sockaddr_in6 *) addr)->sin6_addr;
        addr->ip_length = sizeof(struct in6_addr);
        addr->port = ntohs(((struct sockaddr_in6 *) addr)->sin6_port);
    } else if (addr->mem.ss_family == AF_INET) {
        addr->ip = (char *) &((struct sockaddr_in *) addr)->sin_addr;
        addr->ip_length = sizeof(struct in_addr);
        addr->port = ntohs(((struct sockaddr_in *) addr)->sin_port);
    } else {
        addr->ip_length = 0;
        addr->port = -1;
    }
}

int bsd_local_addr(LIBUS_SOCKET_DESCRIPTOR fd, struct bsd_addr_t *addr) {
    addr->len = sizeof(addr->mem);
    if (getsockname(fd, (struct sockaddr *) &addr->mem, &addr->len)) {
        return -1;
    }
    internal_finalize_bsd_addr(addr);
    return 0;
}

int bsd_remote_addr(LIBUS_SOCKET_DESCRIPTOR fd, struct bsd_addr_t *addr) {
    addr->len = sizeof(addr->mem);
    if (getpeername(fd, (struct sockaddr *) &addr->mem, &addr->len)) {
        return -1;
    }
    internal_finalize_bsd_addr(addr);
    return 0;
}

char *bsd_addr_get_ip(struct bsd_addr_t *addr) {
    return addr->ip;
}

int bsd_addr_get_ip_length(struct bsd_addr_t *addr) {
    return addr->ip_length;
}

int bsd_addr_get_port(struct bsd_addr_t *addr) {
    return addr->port;
}

// called by dispatch_ready_poll
LIBUS_SOCKET_DESCRIPTOR bsd_accept_socket(LIBUS_SOCKET_DESCRIPTOR fd, struct bsd_addr_t *addr) {
    LIBUS_SOCKET_DESCRIPTOR accepted_fd;
    addr->len = sizeof(addr->mem);

#if defined(SOCK_CLOEXEC) && defined(SOCK_NONBLOCK)
    // Linux, FreeBSD
    accepted_fd = accept4(fd, (struct sockaddr *) addr, &addr->len, SOCK_CLOEXEC | SOCK_NONBLOCK);
#else
    // Windows, OS X
    accepted_fd = accept(fd, (struct sockaddr *) addr, &addr->len);

#endif

    /* We cannot rely on addr since it is not initialized if failed */
    if (accepted_fd == LIBUS_SOCKET_ERROR) {
        return LIBUS_SOCKET_ERROR;
    }

    internal_finalize_bsd_addr(addr);

    return bsd_set_nonblocking(apple_no_sigpipe(accepted_fd));
}

int bsd_recv(LIBUS_SOCKET_DESCRIPTOR fd, void *buf, int length, int flags) {
    return recv(fd, buf, length, flags);
}

#ifndef MSG_NOSIGNAL
#define MSG_NOSIGNAL 0
#endif

#if !defined(_WIN32)
#include <sys/uio.h>

int bsd_write2(LIBUS_SOCKET_DESCRIPTOR fd, const char *header, int header_length, const char *payload, int payload_length) {
    struct iovec chunks[2];

    chunks[0].iov_base = (char *)header;
    chunks[0].iov_len = header_length;
    chunks[1].iov_base = (char *)payload;
    chunks[1].iov_len = payload_length;

    struct msghdr msg = {0};
    msg.msg_iov = chunks;
    msg.msg_iovlen = 2;

    return sendmsg(fd, &msg, MSG_NOSIGNAL);
}
#else
int bsd_write2(LIBUS_SOCKET_DESCRIPTOR fd, const char *header, int header_length, const char *payload, int payload_length) {
    int written = bsd_send(fd, header, header_length, 0);
    if (written == header_length) {
        int second_write = bsd_send(fd, payload, payload_length, 0);
        if (second_write > 0) {
            written += second_write;
        }
    }
    return written;
}
#endif

int bsd_send(LIBUS_SOCKET_DESCRIPTOR fd, const char *buf, int length, int msg_more) {

    // MSG_MORE (Linux), MSG_PARTIAL (Windows), TCP_NOPUSH (BSD)

#ifdef MSG_MORE

    // for Linux we do not want signals
    return send(fd, buf, length, ((msg_more != 0) * MSG_MORE) | MSG_NOSIGNAL);

#else

    // use TCP_NOPUSH

    return send(fd, buf, length, MSG_NOSIGNAL);

#endif
}

int bsd_would_block() {
#ifdef _WIN32
    return WSAGetLastError() == WSAEWOULDBLOCK;
#else
    return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR;
#endif
}

int bsd_normalize_socket_error(int error) {
#ifdef _WIN32
    /* Translate the Winsock codes a connect failure can surface into their POSIX errno equivalents so the
     * µWS layer's errno-name mapping (which uses POSIX names) reports the right code on Windows. */
    switch (error) {
        case WSAECONNREFUSED:  return ECONNREFUSED;
        case WSAETIMEDOUT:     return ETIMEDOUT;
        case WSAECONNRESET:    return ECONNRESET;
        case WSAECONNABORTED:  return ECONNABORTED;
        case WSAEHOSTUNREACH:  return EHOSTUNREACH;
        case WSAENETUNREACH:   return ENETUNREACH;
        case WSAENETDOWN:      return ENETDOWN;
        case WSAEMFILE:        return EMFILE;
        case WSAENOBUFS:       return ENOBUFS;
        case WSAEINVAL:        return EINVAL;
        case WSAEAFNOSUPPORT:  return EAFNOSUPPORT;
        case WSAEADDRNOTAVAIL: return EADDRNOTAVAIL;
        case WSAEADDRINUSE:    return EADDRINUSE;
        case WSAEACCES:        return EACCES;
        default:               return error;
    }
#else
    return error;
#endif
}

int bsd_socket_last_error() {
#ifdef _WIN32
    return WSAGetLastError();
#else
    return errno;
#endif
}

// return LIBUS_SOCKET_ERROR or the fd that represents listen socket
// listen both on ipv6 and ipv4
LIBUS_SOCKET_DESCRIPTOR bsd_create_listen_socket(const char *host, int port, int options) {
    struct addrinfo hints, *result;
    memset(&hints, 0, sizeof(struct addrinfo));

    hints.ai_flags = AI_PASSIVE;
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_string[16];
    snprintf(port_string, 16, "%d", port);

    if (getaddrinfo(host, port_string, &hints, &result)) {
        return LIBUS_SOCKET_ERROR;
    }

    LIBUS_SOCKET_DESCRIPTOR listenFd = LIBUS_SOCKET_ERROR;
    struct addrinfo *listenAddr;
    for (struct addrinfo *a = result; a && listenFd == LIBUS_SOCKET_ERROR; a = a->ai_next) {
        if (a->ai_family == AF_INET6) {
            listenFd = bsd_create_socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            listenAddr = a;
        }
    }

    for (struct addrinfo *a = result; a && listenFd == LIBUS_SOCKET_ERROR; a = a->ai_next) {
        if (a->ai_family == AF_INET) {
            listenFd = bsd_create_socket(a->ai_family, a->ai_socktype, a->ai_protocol);
            listenAddr = a;
        }
    }

    if (listenFd == LIBUS_SOCKET_ERROR) {
        freeaddrinfo(result);
        return LIBUS_SOCKET_ERROR;
    }

    /* Otherwise, always enable SO_REUSEPORT and SO_REUSEADDR _unless_ options specify otherwise */
#ifdef _WIN32
    if (options & LIBUS_LISTEN_EXCLUSIVE_PORT) {
        int optval2 = 1;
        setsockopt(listenFd, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (void *) &optval2, sizeof(optval2));
    } else {
        int optval3 = 1;
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, (void *) &optval3, sizeof(optval3));
    }
#else
    #if /*defined(__linux) &&*/ defined(SO_REUSEPORT)
    if (!(options & LIBUS_LISTEN_EXCLUSIVE_PORT)) {
        int optval = 1;
        setsockopt(listenFd, SOL_SOCKET, SO_REUSEPORT, (void *) &optval, sizeof(optval));
    }
    #endif
    int enabled = 1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, (void *) &enabled, sizeof(enabled));
#endif
    
#ifdef IPV6_V6ONLY
    int disabled = 0;
    setsockopt(listenFd, IPPROTO_IPV6, IPV6_V6ONLY, (void *) &disabled, sizeof(disabled));
#endif

    if (bind(listenFd, listenAddr->ai_addr, (socklen_t) listenAddr->ai_addrlen) || listen(listenFd, SOMAXCONN)) {
        bsd_close_socket(listenFd);
        freeaddrinfo(result);
        return LIBUS_SOCKET_ERROR;
    }

    freeaddrinfo(result);
    return listenFd;
}

#ifndef _WIN32
#include <sys/un.h>
#else
#include <afunix.h>
#include <io.h>
#endif
#include <sys/stat.h>
#include <stddef.h>
LIBUS_SOCKET_DESCRIPTOR bsd_create_listen_socket_unix(const char *path, int options) {

    LIBUS_SOCKET_DESCRIPTOR listenFd = LIBUS_SOCKET_ERROR;
    struct sockaddr_un server_address;
    size_t path_length = strlen(path);

    if (path_length >= sizeof(server_address.sun_path)) {
        return LIBUS_SOCKET_ERROR;
    }

    listenFd = bsd_create_socket(AF_UNIX, SOCK_STREAM, 0);

    if (listenFd == LIBUS_SOCKET_ERROR) {
        return LIBUS_SOCKET_ERROR;
    }

#ifndef _WIN32
    // 700 permission by default
    fchmod(listenFd, S_IRWXU);
#else
    _chmod(path, S_IREAD | S_IWRITE | S_IEXEC);
#endif

    memset(&server_address, 0, sizeof(server_address));
    server_address.sun_family = AF_UNIX;
    memcpy(server_address.sun_path, path, path_length + 1);
    int size = offsetof(struct sockaddr_un, sun_path) + path_length;
#ifdef _WIN32
    _unlink(path);
#else
    unlink(path);
#endif

    if (bind(listenFd, (struct sockaddr *)&server_address, size) || listen(listenFd, SOMAXCONN)) {
        bsd_close_socket(listenFd);
        return LIBUS_SOCKET_ERROR;
    }

    return listenFd;
}

LIBUS_SOCKET_DESCRIPTOR bsd_create_connect_socket_addr(const struct sockaddr *addr, socklen_t addrlen, const char *source_host) {
    /* The address is already resolved by the caller; no blocking getaddrinfo here */
    LIBUS_SOCKET_DESCRIPTOR fd = bsd_create_socket(addr->sa_family, SOCK_STREAM, 0);
    if (fd == LIBUS_SOCKET_ERROR) {
        return LIBUS_SOCKET_ERROR;
    }

    if (source_host) {
        struct addrinfo hints, *interface_result;
        memset(&hints, 0, sizeof(hints));
        hints.ai_flags = AI_NUMERICHOST;
        hints.ai_family = addr->sa_family;
        hints.ai_socktype = SOCK_STREAM;

        if (getaddrinfo(source_host, NULL, &hints, &interface_result)) {
            bsd_close_socket(fd);
#ifdef _WIN32
            WSASetLastError(WSAEADDRNOTAVAIL);
#else
            errno = EADDRNOTAVAIL;
#endif
            return LIBUS_SOCKET_ERROR;
        }

        if (bind(fd, interface_result->ai_addr, (socklen_t) interface_result->ai_addrlen) == LIBUS_SOCKET_ERROR) {
            freeaddrinfo(interface_result);
            bsd_close_socket(fd);
#ifdef _WIN32
            WSASetLastError(WSAEADDRNOTAVAIL);
#else
            errno = EADDRNOTAVAIL;
#endif
            return LIBUS_SOCKET_ERROR;
        }
        freeaddrinfo(interface_result);
    }

#ifdef _WIN32
    u_long one = 1;
    ioctlsocket(fd, FIONBIO, &one);

    /* Match libuv's TCP loopback behavior: Windows otherwise retries a refused local connection
     * for two seconds, consuming short connect deadlines before address fallback can run. */
    if ((addr->sa_family == AF_INET && (ntohl(((const struct sockaddr_in *) addr)->sin_addr.s_addr) >> 24) == 127) ||
        (addr->sa_family == AF_INET6 && IN6_IS_ADDR_LOOPBACK(&((const struct sockaddr_in6 *) addr)->sin6_addr))) {
        TCP_INITIAL_RTO_PARAMETERS rto = {0};
        DWORD bytes;
        rto.Rtt = TCP_INITIAL_RTO_DEFAULT_RTT;
        rto.MaxSynRetransmissions = TCP_INITIAL_RTO_NO_SYN_RETRANSMISSIONS;
        WSAIoctl(fd, SIO_TCP_INITIAL_RTO, &rto, sizeof(rto), NULL, 0, &bytes, NULL, NULL);
    }
#endif

    int connect_result = connect(fd, addr, addrlen);
    if (connect_result == 0 ||
#ifdef _WIN32
        (connect_result == SOCKET_ERROR && bsd_socket_last_error() == WSAEWOULDBLOCK)) {
#else
        (connect_result == LIBUS_SOCKET_ERROR && bsd_socket_last_error() == EINPROGRESS)) {
#endif
        return fd;
    }

    bsd_close_socket(fd);
    return LIBUS_SOCKET_ERROR;
}
