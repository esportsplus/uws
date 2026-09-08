#ifndef LIBUSOCKETS_H
#define LIBUSOCKETS_H

/* 512kb shared receive buffer */
#ifndef LIBUS_RECV_BUFFER_LENGTH
#define LIBUS_RECV_BUFFER_LENGTH 524288
#endif

/* A timeout granularity of 4 seconds means give or take 4 seconds from set timeout */
#define LIBUS_TIMEOUT_GRANULARITY 4
/* 32 byte padding of receive buffer ends */
#define LIBUS_RECV_BUFFER_PADDING 32
/* Guaranteed alignment of extension memory */
#define LIBUS_EXT_ALIGNMENT 16

/* Define what a socket descriptor is based on platform */
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#define LIBUS_SOCKET_DESCRIPTOR SOCKET
#else
#define LIBUS_SOCKET_DESCRIPTOR int
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    /* No meaning, default listen option */
    LIBUS_LISTEN_DEFAULT,
    /* We exclusively own this port, do not share it */
    LIBUS_LISTEN_EXCLUSIVE_PORT
};

/* Library types publicly available */
struct us_socket_t;
struct us_timer_t;
struct us_socket_context_t;
struct us_loop_t;
struct us_poll_t;

/* Opaque async connect request; owns DNS resolution, address fallback and the connect/handshake
 * timer. See us_socket_context_connect below and connect.c. */
struct us_connect_request_t;

/* Options for an outbound connection. host/port are always the immediate TCP peer (the proxy, when
 * one is configured in a higher layer); sni_hostname is always the final destination. */
struct us_connect_options_t {
    const char *host;          /* name or literal */
    int port;
    const char *source_host;   /* optional bind */
    const char *sni_hostname;  /* optional, SSL only; defaults to host. Always the DESTINATION, never a proxy */
    int timeout_ms;            /* 0 = none; covers DNS + TCP connect */
    int handshake_timeout_ms;  /* 0 = none; re-arms the same timer at on_open, until release */
    int client_verify;         /* SSL only, per-socket verify override: 0 = inherit the context default,
                                * 1 = force SSL_VERIFY_PEER, 2 = force SSL_VERIFY_NONE. 0 by default. */
};

/* Extra for io_uring */
char *us_socket_send_buffer(int ssl, struct us_socket_t *s);

/* Public interfaces for timers */

/* Create a new high precision, low performance timer. May fail and return null */
struct us_timer_t *us_create_timer(struct us_loop_t *loop, int fallthrough, unsigned int ext_size);

/* Returns user data extension for this timer */
void *us_timer_ext(struct us_timer_t *timer);

/* */
void us_timer_close(struct us_timer_t *timer);

/* Arm a timer with a delay from now and eventually a repeat delay.
 * Specify 0 as repeat delay to disable repeating. Specify both 0 to disarm. */
void us_timer_set(struct us_timer_t *timer, void (*cb)(struct us_timer_t *t), int ms, int repeat_ms);

/* Returns the loop for this timer */
struct us_loop_t *us_timer_loop(struct us_timer_t *t);

/* Public interfaces for contexts */

struct us_socket_context_options_t {
    const char *key_file_name;
    const char *cert_file_name;
    const char *passphrase;
    const char *dh_params_file_name;
    const char *ca_file_name;
    const char *ssl_ciphers;
    int ssl_prefer_low_memory_usage; /* Todo: rename to prefer_low_memory_usage and apply for TCP as well */
    const char *ca_pem;              /* in-memory PEM CA bundle; alternative/addition to ca_file_name (client) */
    int client_mode;                 /* 0 = server context (default, today's behaviour), 1 = client context */
    int reject_unauthorized;         /* client only; 1 = SSL_VERIFY_PEER, 0 = SSL_VERIFY_NONE */
};

/* Return 15-bit timestamp for this context */
unsigned short us_socket_context_timestamp(int ssl, struct us_socket_context_t *context);

/* Adds SNI domain and cert in asn1 format */
int us_socket_context_add_server_name(int ssl, struct us_socket_context_t *context, const char *hostname_pattern, struct us_socket_context_options_t options, void *user);
void us_socket_context_remove_server_name(int ssl, struct us_socket_context_t *context, const char *hostname_pattern);
void us_socket_context_clear_server_name_userdata(int ssl, struct us_socket_context_t *context, const char *hostname_pattern);
void us_socket_context_on_server_name(int ssl, struct us_socket_context_t *context, void (*cb)(struct us_socket_context_t *, const char *hostname));
void *us_socket_server_name_userdata(int ssl, struct us_socket_t *s);
void *us_socket_context_find_server_name_userdata(int ssl, struct us_socket_context_t *context, const char *hostname_pattern);

/* Returns the underlying SSL native handle, such as SSL_CTX or nullptr */
void *us_socket_context_get_native_handle(int ssl, struct us_socket_context_t *context);

/* A socket context holds shared callbacks and user data extension for associated sockets */
struct us_socket_context_t *us_create_socket_context(int ssl, struct us_loop_t *loop,
    int ext_size, struct us_socket_context_options_t options);

/* Delete resources allocated at creation time. */
void us_socket_context_free(int ssl, struct us_socket_context_t *context);

/* Setters of various async callbacks */
void us_socket_context_on_pre_open(int ssl, struct us_socket_context_t *context,
    LIBUS_SOCKET_DESCRIPTOR (*on_pre_open)(struct us_socket_context_t *context, LIBUS_SOCKET_DESCRIPTOR fd, char *ip, int ip_length));
void us_socket_context_on_open(int ssl, struct us_socket_context_t *context,
    struct us_socket_t *(*on_open)(struct us_socket_t *s, int is_client, char *ip, int ip_length));
void us_socket_context_on_close(int ssl, struct us_socket_context_t *context,
    struct us_socket_t *(*on_close)(struct us_socket_t *s, int code, void *reason));
void us_socket_context_on_data(int ssl, struct us_socket_context_t *context,
    struct us_socket_t *(*on_data)(struct us_socket_t *s, char *data, int length));
void us_socket_context_on_writable(int ssl, struct us_socket_context_t *context,
    struct us_socket_t *(*on_writable)(struct us_socket_t *s));
void us_socket_context_on_timeout(int ssl, struct us_socket_context_t *context,
    struct us_socket_t *(*on_timeout)(struct us_socket_t *s));
void us_socket_context_on_long_timeout(int ssl, struct us_socket_context_t *context,
    struct us_socket_t *(*on_timeout)(struct us_socket_t *s));
/* This one is only used for when a connecting socket fails in a late stage. */
void us_socket_context_on_connect_error(int ssl, struct us_socket_context_t *context,
    struct us_socket_t *(*on_connect_error)(struct us_socket_t *s, int code));

/* Emitted when a socket has been half-closed */
void us_socket_context_on_end(int ssl, struct us_socket_context_t *context, struct us_socket_t *(*on_end)(struct us_socket_t *s));

/* Returns user data extension for this socket context */
void *us_socket_context_ext(int ssl, struct us_socket_context_t *context);

/* Closes all open sockets, including listen sockets. Does not invalidate the socket context. */
void us_socket_context_close(int ssl, struct us_socket_context_t *context);

/* Internal teardown helper for low-priority sockets temporarily outside the context socket list. */
void us_internal_socket_context_close_low_prio(int ssl, struct us_socket_context_t *context);

/* Listen for connections. Acts as the main driving cog in a server. Will call set async callbacks. */
struct us_listen_socket_t *us_socket_context_listen(int ssl, struct us_socket_context_t *context,
    const char *host, int port, int options, int socket_ext_size);

struct us_listen_socket_t *us_socket_context_listen_unix(int ssl, struct us_socket_context_t *context,
    const char *path, int options, int socket_ext_size);

/* listen_socket.c/.h */
void us_listen_socket_close(int ssl, struct us_listen_socket_t *ls);

/* Adopt a socket which was accepted either internally, or from another accept() outside libusockets */
struct us_socket_t *us_adopt_accepted_socket(int ssl, struct us_socket_context_t *context, LIBUS_SOCKET_DESCRIPTOR client_fd,
    unsigned int socket_ext_size, char *addr_ip, int addr_ip_length);

/* Is this socket established? Can be used to check if a connecting socket has fired the on_open event yet.
 * Can also be used to determine if a socket is a listen_socket or not, but you probably know that already. */
int us_socket_is_established(int ssl, struct us_socket_t *s);

/* Cancel a connecting socket. Can be used together with us_socket_timeout to limit connection times.
 * Entirely destroys the socket - this function works like us_socket_close but does not trigger on_close event since
 * you never got the on_open event first. */
struct us_socket_t *us_socket_close_connecting(int ssl, struct us_socket_t *s);

/* Start an outbound connection: resolve options->host asynchronously, then walk the resolved
 * addresses until one connects, then fire on_open(s, 1, 0, 0) - or on_connect_error(s, code) if
 * resolution fails or every address is exhausted. Returns NULL only on immediate argument failure;
 * otherwise a request handle that frees itself on on_connect_error and lives (state HANDSHAKING,
 * owning the ms timer) past on_open until us_connect_request_release. */
struct us_connect_request_t *us_socket_context_connect(int ssl, struct us_socket_context_t *context,
    const struct us_connect_options_t *options, int socket_ext_size);

/* Abort a pending request; no callbacks fire after this returns. */
void us_connect_request_cancel(int ssl, struct us_connect_request_t *r);

/* After on_open: the socket was moved by us_socket_context_adopt_socket (realloc); point the request
 * at the new socket and re-link it onto the new context's request list. */
void us_connect_request_rebind(struct us_connect_request_t *r, struct us_socket_t *s);

/* After on_open: close the handshake timer and free the request. Called by the owner of the socket on
 * adoption into the frame phase, or on any close before that. The caller nulls its pointer. */
void us_connect_request_release(struct us_connect_request_t *r);

/* Returns the loop for this socket context. */
struct us_loop_t *us_socket_context_loop(int ssl, struct us_socket_context_t *context);

/* Invalidates passed socket, returning a new resized socket which belongs to a different socket context.
 * Used mainly for "socket upgrades" such as when transitioning from HTTP to WebSocket. */
struct us_socket_t *us_socket_context_adopt_socket(int ssl, struct us_socket_context_t *context, struct us_socket_t *s, int ext_size);

/* Start TLS on a socket that was opened PLAINTEXT. Precondition: the caller has already adopted s into an
 * SSL context with us_socket_context_adopt_socket(1, ...), so its ext has room for the
 * us_internal_ssl_socket_t overlay. Zeroes the overlay, SSL_new from the context, client mode, SNI +
 * SSL_set1_host + ALPN http/1.1 from sni, and emits the ClientHello. From there ssl_on_data drives the
 * handshake as for any client socket. Returns s. */
struct us_socket_t *us_socket_start_tls(struct us_socket_t *s, const char *sni, int client_verify);

/* Create a child socket context which acts much like its own socket context with its own callbacks yet still relies on the
 * parent socket context for some shared resources. Child socket contexts should be used together with socket adoptions and nothing else. */
struct us_socket_context_t *us_create_child_socket_context(int ssl, struct us_socket_context_t *context, int context_ext_size);

/* Public interfaces for loops */

/* Returns a new event loop with user data extension */
struct us_loop_t *us_create_loop(void *hint, void (*wakeup_cb)(struct us_loop_t *loop),
    void (*pre_cb)(struct us_loop_t *loop), void (*post_cb)(struct us_loop_t *loop), unsigned int ext_size);

/* Frees the loop immediately */
void us_loop_free(struct us_loop_t *loop);

/* Returns the loop user data extension */
void *us_loop_ext(struct us_loop_t *loop);

/* Blocks the calling thread and drives the event loop until no more non-fallthrough polls are scheduled */
void us_loop_run(struct us_loop_t *loop);

/* Blocks for one event-loop iteration. */
void us_loop_run_once(struct us_loop_t *loop);

/* Returns the number of in-flight asynchronous DNS resolutions owned by this loop. */
int us_internal_loop_pending_resolves(struct us_loop_t *loop);

/* Signals the loop from any thread to wake up and execute its wakeup handler from the loop's own running thread.
 * This is the only fully thread-safe function and serves as the basis for thread safety */
void us_wakeup_loop(struct us_loop_t *loop);

/* Hook up timers in existing loop */
void us_loop_integrate(struct us_loop_t *loop);

/* Returns the loop iteration number */
long long us_loop_iteration_number(struct us_loop_t *loop);

/* Public interfaces for polls */

/* A fallthrough poll does not keep the loop running, it falls through */
struct us_poll_t *us_create_poll(struct us_loop_t *loop, int fallthrough, unsigned int ext_size);

/* After stopping a poll you must manually free the memory */
void us_poll_free(struct us_poll_t *p, struct us_loop_t *loop);

/* Associate this poll with a socket descriptor and poll type */
void us_poll_init(struct us_poll_t *p, LIBUS_SOCKET_DESCRIPTOR fd, int poll_type);

/* Start, change and stop polling for events */
void us_poll_start(struct us_poll_t *p, struct us_loop_t *loop, int events);
void us_poll_change(struct us_poll_t *p, struct us_loop_t *loop, int events);
void us_poll_stop(struct us_poll_t *p, struct us_loop_t *loop);

/* Return what events we are polling for */
int us_poll_events(struct us_poll_t *p);

/* Returns the user data extension of this poll */
void *us_poll_ext(struct us_poll_t *p);

/* Get associated socket descriptor from a poll */
LIBUS_SOCKET_DESCRIPTOR us_poll_fd(struct us_poll_t *p);

/* Resize an active poll */
struct us_poll_t *us_poll_resize(struct us_poll_t *p, struct us_loop_t *loop, unsigned int ext_size);

/* Public interfaces for sockets */

/* Returns the underlying native handle for a socket, such as SSL or file descriptor.
 * In the case of file descriptor, the value of pointer is fd. */
void *us_socket_get_native_handle(int ssl, struct us_socket_t *s);

/* Write up to length bytes of data. Returns actual bytes written.
 * Will call the on_writable callback of active socket context on failure to write everything off in one go.
 * Set hint msg_more if you have more immediate data to write. */
int us_socket_write(int ssl, struct us_socket_t *s, const char *data, int length, int msg_more);

/* Special path for non-SSL sockets. Used to send header and payload in one go. Works like us_socket_write. */
int us_socket_write2(int ssl, struct us_socket_t *s, const char *header, int header_length, const char *payload, int payload_length);

/* Set a low precision, high performance timer on a socket. A socket can only have one single active timer
 * at any given point in time. Will remove any such pre set timer */
void us_socket_timeout(int ssl, struct us_socket_t *s, unsigned int seconds);

/* Set a low precision, high performance timer on a socket. Suitable for per-minute precision. */
void us_socket_long_timeout(int ssl, struct us_socket_t *s, unsigned int minutes);

/* Return the user data extension of this socket */
void *us_socket_ext(int ssl, struct us_socket_t *s);

/* Return the socket context of this socket */
struct us_socket_context_t *us_socket_context(int ssl, struct us_socket_t *s);

/* Withdraw any msg_more status and flush any pending data */
void us_socket_flush(int ssl, struct us_socket_t *s);

/* Shuts down the connection by sending FIN and/or close_notify */
void us_socket_shutdown(int ssl, struct us_socket_t *s);

/* Shuts down the connection in terms of read, meaning next event loop
 * iteration will catch the socket being closed. Can be used to defer closing
 * to next event loop iteration. */
void us_socket_shutdown_read(int ssl, struct us_socket_t *s);

/* Returns whether the socket has been shut down or not */
int us_socket_is_shut_down(int ssl, struct us_socket_t *s);

/* Returns whether this socket has been closed. Only valid if memory has not yet been released. */
int us_socket_is_closed(int ssl, struct us_socket_t *s);

/* Immediately closes the socket */
struct us_socket_t *us_socket_close(int ssl, struct us_socket_t *s, int code, void *reason);

/* Returns local port or -1 on failure. */
int us_socket_local_port(int ssl, struct us_socket_t *s);

/* Returns remote ephemeral port or -1 on failure. */
int us_socket_remote_port(int ssl, struct us_socket_t *s);

/* Copy remote (IP) address of socket, or fail with zero length. */
void us_socket_remote_address(int ssl, struct us_socket_t *s, char *buf, int *length);

#ifdef __cplusplus
}
#endif

/* This build always uses the libuv backend */
#ifndef LIBUS_USE_LIBUV
#define LIBUS_USE_LIBUV
#endif

#endif // LIBUSOCKETS_H
