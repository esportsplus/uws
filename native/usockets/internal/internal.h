#ifndef INTERNAL_H
#define INTERNAL_H

#if defined(_MSC_VER)
#define alignas(x) __declspec(align(x))
#else
#include <stdalign.h>
#endif

/* We only have one networking implementation so far */
#include "internal/networking/bsd.h"

// WARNING: This MUST be included after bsd.h, otherwise sys/socket includes
// fail surprisingly.
#include <stdint.h>

#ifndef LIBUS_USE_IO_URING

#ifdef LIBUS_USE_LIBUV
#include "internal/eventing/libuv.h"
#endif

/* Poll type and what it polls for */
enum {
    /* Two first bits */
    POLL_TYPE_SOCKET = 0,
    POLL_TYPE_SOCKET_SHUT_DOWN = 1,
    POLL_TYPE_SEMI_SOCKET = 2,
    POLL_TYPE_CALLBACK = 3,

    /* Two last bits */
    POLL_TYPE_POLLING_OUT = 4,
    POLL_TYPE_POLLING_IN = 8
};

/* Loop related */
void us_internal_dispatch_ready_poll(struct us_poll_t *p, int error, int events);
void us_internal_timer_sweep(struct us_loop_t *loop);
void us_internal_free_closed_sockets(struct us_loop_t *loop);
void us_internal_loop_link(struct us_loop_t *loop, struct us_socket_context_t *context);
void us_internal_loop_unlink(struct us_loop_t *loop, struct us_socket_context_t *context);
void us_internal_loop_data_init(struct us_loop_t *loop, void (*wakeup_cb)(struct us_loop_t *loop),
    void (*pre_cb)(struct us_loop_t *loop), void (*post_cb)(struct us_loop_t *loop));
void us_internal_loop_data_free(struct us_loop_t *loop);
void us_internal_loop_pre(struct us_loop_t *loop);
void us_internal_loop_post(struct us_loop_t *loop);

/* Asyncs (old) */
struct us_internal_async *us_internal_create_async(struct us_loop_t *loop, int fallthrough, unsigned int ext_size);
void us_internal_async_close(struct us_internal_async *a);
void us_internal_async_set(struct us_internal_async *a, void (*cb)(struct us_internal_async *));
void us_internal_async_wakeup(struct us_internal_async *a);

/* Eventing related */
unsigned int us_internal_accept_poll_event(struct us_poll_t *p);
int us_internal_poll_type(struct us_poll_t *p);
void us_internal_poll_set_type(struct us_poll_t *p, int poll_type);

/* SSL loop data */
void us_internal_init_loop_ssl_data(struct us_loop_t *loop);
void us_internal_free_loop_ssl_data(struct us_loop_t *loop);
void us_internal_ssl_free_pending(struct us_loop_t *loop);

/* Socket context related */
void us_internal_socket_context_link_socket(struct us_socket_context_t *context, struct us_socket_t *s);
void us_internal_socket_context_unlink_socket(struct us_socket_context_t *context, struct us_socket_t *s);

/* Connect state machine (connect.c). The generic implementation keyed on the base context; the
 * SSL path (step 5) wraps this with a size-adjusting us_internal_ssl_socket_context_connect. */
struct us_connect_request_t *us_internal_socket_context_connect(struct us_socket_context_t *context,
    const struct us_connect_options_t *options, int socket_ext_size, int is_ssl);
/* Drives a connect semi-socket that just became writable: err != 0 means this address failed (try the
 * next), err == 0 means the connection is up (promote to a full socket and fire on_open). */
struct us_socket_t *us_internal_socket_connect_ready(struct us_socket_t *s, int err);
/* Called from us_socket_context_free for every request still bound to the context being torn down. */
void us_internal_connect_request_orphan(struct us_connect_request_t *r);
/* Read the next request in a context's intrusive list (the struct is private to connect.c). Must be
 * called before us_internal_connect_request_orphan, which may free the request. */
struct us_connect_request_t *us_internal_connect_request_next(struct us_connect_request_t *r);

/* Sockets are polls */
struct us_socket_t {
    alignas(LIBUS_EXT_ALIGNMENT) struct us_poll_t p; // 4 bytes
    unsigned char timeout; // 1 byte
    unsigned char long_timeout; // 1 byte
    unsigned short low_prio_state; /* 0 = not in low-prio queue, 1 = is in low-prio queue, 2 = was in low-prio queue in this iteration */
    struct us_socket_context_t *context;
    struct us_socket_t *prev, *next;
};

/* Internal callback types are polls just like sockets */
struct us_internal_callback_t {
    alignas(LIBUS_EXT_ALIGNMENT) struct us_poll_t p;
    struct us_loop_t *loop;
    int cb_expects_the_loop;
    void (*cb)(struct us_internal_callback_t *cb);
};

/* Listen sockets are sockets */
struct us_listen_socket_t {
    alignas(LIBUS_EXT_ALIGNMENT) struct us_socket_t s;
    unsigned int socket_ext_size;
};

/* Listen sockets are kept in their own list */
void us_internal_socket_context_link_listen_socket(struct us_socket_context_t *context, struct us_listen_socket_t *s);
void us_internal_socket_context_unlink_listen_socket(struct us_socket_context_t *context, struct us_listen_socket_t *s);

struct us_socket_context_t {
    alignas(LIBUS_EXT_ALIGNMENT) struct us_loop_t *loop;
    uint32_t global_tick;
    unsigned char timestamp;
    unsigned char long_timestamp;
    struct us_socket_t *head_sockets;
    struct us_listen_socket_t *head_listen_sockets;
    struct us_socket_t *iterator;
    struct us_socket_context_t *prev, *next;

    LIBUS_SOCKET_DESCRIPTOR (*on_pre_open)(struct us_socket_context_t *context, LIBUS_SOCKET_DESCRIPTOR fd, char *ip, int ip_length);
    struct us_socket_t *(*on_open)(struct us_socket_t *, int is_client, char *ip, int ip_length);
    struct us_socket_t *(*on_data)(struct us_socket_t *, char *data, int length);
    struct us_socket_t *(*on_writable)(struct us_socket_t *);
    struct us_socket_t *(*on_close)(struct us_socket_t *, int code, void *reason);
    //void (*on_timeout)(struct us_socket_context *);
    struct us_socket_t *(*on_socket_timeout)(struct us_socket_t *);
    struct us_socket_t *(*on_socket_long_timeout)(struct us_socket_t *);
    struct us_socket_t *(*on_end)(struct us_socket_t *);
    struct us_socket_t *(*on_connect_error)(struct us_socket_t *, int code);
    int (*is_low_prio)(struct us_socket_t *);

    /* Intrusive head of the in-flight connect requests owned by this context (connect.c). */
    struct us_connect_request_t *head_connect_requests;
};

#endif

/* Internal SSL interface */
#ifndef LIBUS_NO_SSL

struct us_internal_ssl_socket_context_t;
struct us_internal_ssl_socket_t;

/* SNI functions */
void us_internal_ssl_socket_context_add_server_name(struct us_internal_ssl_socket_context_t *context, const char *hostname_pattern, struct us_socket_context_options_t options, void *user);
void us_internal_ssl_socket_context_remove_server_name(struct us_internal_ssl_socket_context_t *context, const char *hostname_pattern);
void us_internal_ssl_socket_context_on_server_name(struct us_internal_ssl_socket_context_t *context, void (*cb)(struct us_internal_ssl_socket_context_t *, const char *));
void *us_internal_ssl_socket_get_sni_userdata(struct us_internal_ssl_socket_t *s);
void *us_internal_ssl_socket_context_find_server_name_userdata(struct us_internal_ssl_socket_context_t *context, const char *hostname_pattern);

void *us_internal_ssl_socket_get_native_handle(struct us_internal_ssl_socket_t *s);
void *us_internal_ssl_socket_context_get_native_handle(struct us_internal_ssl_socket_context_t *context);

struct us_internal_ssl_socket_context_t *us_internal_create_ssl_socket_context(struct us_loop_t *loop,
    int context_ext_size, struct us_socket_context_options_t options);

void us_internal_ssl_socket_context_free(struct us_internal_ssl_socket_context_t *context);
void us_internal_ssl_socket_context_on_open(struct us_internal_ssl_socket_context_t *context,
    struct us_internal_ssl_socket_t *(*on_open)(struct us_internal_ssl_socket_t *s, int is_client, char *ip, int ip_length));

void us_internal_ssl_socket_context_on_close(struct us_internal_ssl_socket_context_t *context,
    struct us_internal_ssl_socket_t *(*on_close)(struct us_internal_ssl_socket_t *s, int code, void *reason));

void us_internal_ssl_socket_context_on_data(struct us_internal_ssl_socket_context_t *context,
    struct us_internal_ssl_socket_t *(*on_data)(struct us_internal_ssl_socket_t *s, char *data, int length));

void us_internal_ssl_socket_context_on_writable(struct us_internal_ssl_socket_context_t *context,
    struct us_internal_ssl_socket_t *(*on_writable)(struct us_internal_ssl_socket_t *s));

void us_internal_ssl_socket_context_on_timeout(struct us_internal_ssl_socket_context_t *context,
    struct us_internal_ssl_socket_t *(*on_timeout)(struct us_internal_ssl_socket_t *s));

void us_internal_ssl_socket_context_on_long_timeout(struct us_internal_ssl_socket_context_t *context,
    struct us_internal_ssl_socket_t *(*on_timeout)(struct us_internal_ssl_socket_t *s));

void us_internal_ssl_socket_context_on_end(struct us_internal_ssl_socket_context_t *context,
    struct us_internal_ssl_socket_t *(*on_end)(struct us_internal_ssl_socket_t *s));

void us_internal_ssl_socket_context_on_connect_error(struct us_internal_ssl_socket_context_t *context,
    struct us_internal_ssl_socket_t *(*on_connect_error)(struct us_internal_ssl_socket_t *s, int code));

struct us_listen_socket_t *us_internal_ssl_socket_context_listen(struct us_internal_ssl_socket_context_t *context,
    const char *host, int port, int options, int socket_ext_size);

struct us_listen_socket_t *us_internal_ssl_socket_context_listen_unix(struct us_internal_ssl_socket_context_t *context,
    const char *path, int options, int socket_ext_size);

struct us_internal_ssl_socket_t *us_internal_ssl_adopt_accepted_socket(struct us_internal_ssl_socket_context_t *context, LIBUS_SOCKET_DESCRIPTOR accepted_fd,
    unsigned int socket_ext_size, char *addr_ip, int addr_ip_length);

int us_internal_ssl_socket_write(struct us_internal_ssl_socket_t *s, const char *data, int length, int msg_more);
void us_internal_ssl_socket_timeout(struct us_internal_ssl_socket_t *s, unsigned int seconds);
void *us_internal_ssl_socket_context_ext(struct us_internal_ssl_socket_context_t *s);
struct us_internal_ssl_socket_context_t *us_internal_ssl_socket_get_context(struct us_internal_ssl_socket_t *s);
void *us_internal_ssl_socket_ext(struct us_internal_ssl_socket_t *s);
int us_internal_ssl_socket_is_shut_down(struct us_internal_ssl_socket_t *s);
void us_internal_ssl_socket_shutdown(struct us_internal_ssl_socket_t *s);

struct us_internal_ssl_socket_t *us_internal_ssl_socket_context_adopt_socket(struct us_internal_ssl_socket_context_t *context,
    struct us_internal_ssl_socket_t *s, int ext_size);

struct us_internal_ssl_socket_context_t *us_internal_create_child_ssl_socket_context(struct us_internal_ssl_socket_context_t *context, int context_ext_size);
struct us_loop_t *us_internal_ssl_socket_context_loop(struct us_internal_ssl_socket_context_t *context);

/* SSL connect wrapper: adds the us_internal_ssl_socket_t overlay delta to socket_ext_size (like the
 * listen wrapper) and drives the generic connect state machine with is_ssl = 1. connect.c routes the
 * ssl branch of us_socket_context_connect here. */
struct us_connect_request_t *us_internal_ssl_socket_context_connect(struct us_internal_ssl_socket_context_t *context,
    const struct us_connect_options_t *options, int socket_ext_size);

/* Move ownership of an strdup'd SNI/verification hostname into a promoted SSL client socket's ext
 * overlay (connect.c calls this; ssl_on_close frees it). An accessor so the us_internal_ssl_socket_t
 * layout does not need to leak into connect.c. */
void us_internal_ssl_socket_set_client_sni(struct us_socket_t *s, char *sni);
void us_internal_ssl_socket_set_client_verify(struct us_socket_t *s, int client_verify);

#endif

#endif // INTERNAL_H
