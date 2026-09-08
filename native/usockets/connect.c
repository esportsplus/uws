#ifndef LIBUS_USE_IO_URING

#include "libusockets.h"
#include "internal/internal.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <errno.h>

#ifndef _WIN32
#include <netdb.h>
#endif

/* Async TCP-connect state machine (spec A.3a).
 *
 * A single us_connect_request_t drives one outbound connection attempt from DNS resolution through
 * address fallback to the moment the socket is handed to the owner via on_open. It owns:
 *   - an embedded uv_getaddrinfo_t (threadpool DNS, one allocation with the request),
 *   - the addrinfo result list it walks one address at a time,
 *   - one us_timer_t (ms precision) that covers DNS + connect and is re-armed for the handshake,
 *   - the current attempt socket (a POLL_TYPE_SEMI_SOCKET while connecting, promoted on success),
 *   - an intrusive place on the owning context's request list.
 *
 * The request pointer is stashed in the first sizeof(void*) bytes of a semi-socket's ext (unused by
 * the owner while connecting). The ext is memset to zero on promotion so the owner - and, for SSL,
 * the us_internal_ssl_socket_t overlay's ssl pointer - starts from a clean slate.
 *
 * ORPHANING INVARIANT (ASAN-critical): if the context is freed while a uv_getaddrinfo is in flight,
 * us_socket_context_free calls us_internal_connect_request_orphan, which uv_cancel()s the request and
 * sets context = NULL. libuv still delivers the callback later (UV_ECANCELED); connect_gai_cb sees
 * context == NULL, frees the request and returns without ever dereferencing the freed context. */

enum {
    US_CONNECT_RESOLVING,
    US_CONNECT_CONNECTING,
    US_CONNECT_HANDSHAKING,
    US_CONNECT_DONE,
    US_CONNECT_CANCELLED
};

struct us_connect_request_t {
    struct us_socket_context_t *context;      /* NULL once the context is freed (orphaned) */
    struct us_loop_t *loop;
    uv_getaddrinfo_t gai;                      /* co-allocated, like uv_poll_t in us_create_poll */
    struct addrinfo *results, *cursor;
    struct us_timer_t *timer;                  /* when timeout_ms > 0 or handshake_timeout_ms > 0 */
    struct us_socket_t *attempt;               /* current semi-socket; after promotion, the live socket */
    struct us_connect_request_t *prev, *next;  /* intrusive list on the context */
    int socket_ext_size;
    int handshake_timeout_ms;
    int client_verify;                         /* SSL per-socket verify override (0/1/2); moved to the ext at promotion */
    char *source_host, *sni_hostname;          /* strdup'd; sni ownership moves to the SSL ext at promotion */
    int is_ssl;
    unsigned char state;
};

/* Forward decls of file-locals */
static void connect_gai_cb(uv_getaddrinfo_t *gai, int status, struct addrinfo *res);
static void connect_timer_cb(struct us_timer_t *t);
static void try_next(struct us_connect_request_t *r, int fail_code);
static void connect_fail(struct us_connect_request_t *r, int code);

/* --- request list (intrusive, on the context) --- */

static void link_request(struct us_socket_context_t *context, struct us_connect_request_t *r) {
    r->context = context;
    r->prev = 0;
    r->next = context->head_connect_requests;
    if (context->head_connect_requests) {
        context->head_connect_requests->prev = r;
    }
    context->head_connect_requests = r;
}

/* Safe to call once (idempotent-ish): unlinks from the context list if still linked. No-op when the
 * request has already been orphaned (context == NULL) - orphaned requests are torn out of the list by
 * us_socket_context_free itself, so they must not touch the freed context here. */
static void unlink_request(struct us_connect_request_t *r) {
    if (!r->context) {
        return;
    }
    if (r->prev) {
        r->prev->next = r->next;
    } else if (r->context->head_connect_requests == r) {
        r->context->head_connect_requests = r->next;
    }
    if (r->next) {
        r->next->prev = r->prev;
    }
    r->prev = 0;
    r->next = 0;
}

struct us_connect_request_t *us_internal_connect_request_next(struct us_connect_request_t *r) {
    return r->next;
}

/* --- teardown helpers --- */

/* Free just the request memory and its owned strings. Callers must have already released the timer,
 * the attempt socket and the addrinfo results. */
static void request_free(struct us_connect_request_t *r) {
    free(r->source_host);
    free(r->sni_hostname);
    free(r);
}

static void request_drop_results(struct us_connect_request_t *r) {
    if (r->results) {
        uv_freeaddrinfo(r->results);
        r->results = 0;
        r->cursor = 0;
    }
}

static void request_close_timer(struct us_connect_request_t *r) {
    if (r->timer) {
        /* Closing a timer from inside its own callback is safe: libuv defers the actual close. */
        us_timer_close(r->timer);
        r->timer = 0;
    }
}

/* --- fdless throwaway socket for on_connect_error --- */

/* on_connect_error needs a us_socket_t so the owner can key per-connection state off the ext, but on
 * DNS failure or address exhaustion there is no fd. Allocate a poll with fd = -1 that is never started
 * (its uv handle is never registered), hand it to the callback, then queue it on the loop's closed
 * list so it is freed at the end of this iteration. This socket is never established and must never be
 * written to. */
static void emit_connect_error(struct us_connect_request_t *r, int code) {
    struct us_socket_context_t *context = r->context;
    if (!context || !context->on_connect_error) {
        return;
    }

    struct us_poll_t *p = us_create_poll(context->loop, 0,
        sizeof(struct us_socket_t) - sizeof(struct us_poll_t) + r->socket_ext_size);
    if (!p) {
        return;
    }

    /* The poll is never started, so its uv_poll_t is never uv_poll_init'd. Zero it so the eventual
     * us_poll_free sees a non-closing handle and frees the allocation directly instead of reading
     * uninitialised uv flags. */
    memset(p->uv_p, 0, sizeof(uv_poll_t));
    us_poll_init(p, (LIBUS_SOCKET_DESCRIPTOR) -1, POLL_TYPE_SEMI_SOCKET);

    struct us_socket_t *s = (struct us_socket_t *) p;
    s->context = context;
    s->timeout = 255;
    s->long_timeout = 255;
    s->low_prio_state = 0;

    /* Clean-slate ext so the owner sees zeroed per-connection state (and, for SSL, a NULL ssl ptr). */
    memset(us_socket_ext(0, s), 0, r->socket_ext_size);

    context->on_connect_error(s, code);

    /* Defer freeing to end of loop iteration via the closed list; mark it closed (prev == context). */
    s->next = context->loop->data.closed_head;
    context->loop->data.closed_head = s;
    s->prev = (struct us_socket_t *) context;
}

/* --- failure paths --- */

/* Terminal failure with NO uv_getaddrinfo pending: fire on_connect_error and free the request now.
 * Used for the async gai error (the callback is executing, so the request is no longer pending),
 * address exhaustion, and the connect-phase timeout. */
static void connect_fail(struct us_connect_request_t *r, int code) {
    if (r->attempt) {
        us_socket_close_connecting(r->is_ssl, r->attempt);
        r->attempt = 0;
    }

    emit_connect_error(r, code);

    request_close_timer(r);
    request_drop_results(r);
    unlink_request(r);
    request_free(r);
}

/* --- state machine --- */

/* Try the address at r->cursor, advancing past any that fail to even create a socket. On success the
 * request adopts a semi-socket polling WRITABLE; on exhaustion it fails with fail_code (the last
 * SO_ERROR from a failed connect, or a socket()/bind() errno, or ECONNREFUSED as a default). */
static void try_next(struct us_connect_request_t *r, int fail_code) {
    while (r->cursor) {
        errno = 0;
        LIBUS_SOCKET_DESCRIPTOR fd = bsd_create_connect_socket_addr(r->cursor->ai_addr,
            (socklen_t) r->cursor->ai_addrlen, r->source_host);

        if (fd == LIBUS_SOCKET_ERROR) {
            /* errno is not set by a failed Winsock socket()/bind(); read the platform's real last error
             * and normalize it to POSIX so a bad source_host reports EADDRNOTAVAIL/EACCES cross-platform. */
            int last = bsd_normalize_socket_error(bsd_socket_last_error());
            if (last) {
                fail_code = last;
            }
            r->cursor = r->cursor->ai_next;
            continue;
        }

        /* Connect sockets are semi-sockets, just like listen sockets, but poll for WRITABLE. */
        struct us_poll_t *p = us_create_poll(r->loop, 0,
            sizeof(struct us_socket_t) - sizeof(struct us_poll_t) + r->socket_ext_size);
        us_poll_init(p, fd, POLL_TYPE_SEMI_SOCKET);
        us_poll_start(p, r->loop, LIBUS_SOCKET_WRITABLE);

        struct us_socket_t *s = (struct us_socket_t *) p;

        /* Linked into the context so the sweep can reach it; timeout byte 255 keeps the sweep from
         * ever firing on a connecting socket (the request's own ms timer owns the deadline). */
        s->context = r->context;
        s->timeout = 255;
        s->long_timeout = 255;
        s->low_prio_state = 0;
        us_internal_socket_context_link_socket(r->context, s);

        /* Stash the request in the ext's first sizeof(void*) bytes so the writable event can find it. */
        *((struct us_connect_request_t **) us_socket_ext(0, s)) = r;

        r->attempt = s;
        return;
    }

    /* No address left. */
    connect_fail(r, fail_code);
}

/* DNS resolution completed (or was cancelled). */
static void connect_gai_cb(uv_getaddrinfo_t *gai, int status, struct addrinfo *res) {
    struct us_connect_request_t *r = (struct us_connect_request_t *) gai->data;

    r->loop->data.pending_resolves--;

    /* ORPHANED: the context was freed while we were resolving. Everything else (timer, list) was torn
     * down by us_socket_context_free; just reap the request and the fresh results, touching NOTHING
     * that depends on the freed context. This is the ASAN-critical branch. */
    if (!r->context) {
        if (res) {
            uv_freeaddrinfo(res);
        }
        request_free(r);
        return;
    }

    /* Deferred cancel/timeout: teardown already ran, this late UV_ECANCELED just reaps the request. */
    if (r->state == US_CONNECT_CANCELLED) {
        if (res) {
            uv_freeaddrinfo(res);
        }
        request_free(r);
        return;
    }

    if (status < 0) {
        if (res) {
            uv_freeaddrinfo(res);
        }
        /* Propagate the NEGATIVE libuv EAI_* code (positive == system errno, negative == resolver
         * error, per the on_connect_error contract). The gai is no longer pending (its callback is
         * running), so free now. */
        connect_fail(r, status);
        return;
    }

    r->results = res;
    r->cursor = res;
    r->state = US_CONNECT_CONNECTING;
    try_next(r, ECONNREFUSED);
}

/* Called from loop.c when a connect semi-socket becomes writable. */
struct us_socket_t *us_internal_socket_connect_ready(struct us_socket_t *s, int err) {
    struct us_connect_request_t *r = *((struct us_connect_request_t **) us_socket_ext(0, s));

    if (err) {
        /* This address failed. Close the attempt (no on_close) and move on to the next one. */
        us_socket_close_connecting(r->is_ssl, s);
        r->attempt = 0;
        if (r->cursor) {
            r->cursor = r->cursor->ai_next;
        }
        try_next(r, err);
        return s;
    }

    /* Connected. Promote the semi-socket to a full socket. */
    struct us_poll_t *p = &s->p;

    /* Established sockets poll for readable. */
    us_poll_change(p, r->loop, LIBUS_SOCKET_READABLE);

    /* We always use nodelay. */
    bsd_socket_nodelay(us_poll_fd(p), 1);

    /* We are now a proper socket. */
    us_internal_poll_set_type(p, POLL_TYPE_SOCKET);

    /* Wipe the ext so the owner sees a clean slate before on_open. This clears our stashed request
     * pointer and, for SSL, zeroes the us_internal_ssl_socket_t overlay so ssl is NULL. */
    memset(us_socket_ext(0, s), 0, r->socket_ext_size);

    r->attempt = s;
    r->state = US_CONNECT_HANDSHAKING;

    /* The addresses are no longer needed once one has connected. */
    request_drop_results(r);

    /* Re-arm the shared timer for the handshake phase (spec: one ms timer for connect + handshake),
     * or close it if there is no handshake deadline. Done synchronously BEFORE on_open so a stale
     * connect-phase fire can never land after promotion. */
    if (r->timer) {
        if (r->handshake_timeout_ms > 0) {
            us_timer_set(r->timer, connect_timer_cb, r->handshake_timeout_ms, 0);
        } else {
            request_close_timer(r);
        }
    }

    /* Step 5: move ownership of the SNI/verification hostname into the SSL socket ext's client_sni
     * field, after the memset and before on_open. From here ssl_on_close frees it, so the request must
     * not. Non-SSL requests keep sni_hostname and free it with the request. SNI travels via the ext
     * field, not the on_open ip param. */
#ifndef LIBUS_NO_SSL
    if (r->is_ssl) {
        us_internal_ssl_socket_set_client_sni(s, r->sni_hostname);
        r->sni_hostname = 0;
        us_internal_ssl_socket_set_client_verify(s, r->client_verify);
    }
#endif

    /* Hand the peer address to on_open exactly like the accept path does, so owners that cache the
     * remote address in userspace (UWS_REMOTE_ADDRESS_USERSPACE) see the address that actually
     * connected - which, after address fallback, need not be the first resolved one. */
    struct bsd_addr_t addr;
    char *ip = 0;
    int ip_length = 0;
    if (!bsd_remote_addr(us_poll_fd(p), &addr)) {
        ip = bsd_addr_get_ip(&addr);
        ip_length = bsd_addr_get_ip_length(&addr);
    }

    return s->context->on_open(s, 1, ip, ip_length);
}

/* The request's ms timer fired. */
static void connect_timer_cb(struct us_timer_t *t) {
    struct us_connect_request_t *r = *((struct us_connect_request_t **) us_timer_ext(t));

    if (r->state == US_CONNECT_HANDSHAKING) {
        /* Handshake deadline. Hand the live socket to the same callback the sweep would use; the owner
         * closes it and its on_close calls us_connect_request_release, which closes THIS timer (libuv
         * defers) and frees r. We must not touch r or t after the dispatch. */
        r->state = US_CONNECT_DONE;
        struct us_socket_t *s = r->attempt;
        s->context->on_socket_timeout(s);
        return;
    }

    /* Connect/DNS deadline (RESOLVING or CONNECTING). */
    int gai_pending = (r->state == US_CONNECT_RESOLVING);

    emit_connect_error(r, ETIMEDOUT);

    request_close_timer(r);
    if (r->attempt) {
        us_socket_close_connecting(r->is_ssl, r->attempt);
        r->attempt = 0;
    }
    request_drop_results(r);
    unlink_request(r);

    if (gai_pending) {
        /* A uv_getaddrinfo is still in flight; cancel it and let its (deferred) callback reap r. */
        r->state = US_CONNECT_CANCELLED;
        uv_cancel((uv_req_t *) &r->gai);
    } else {
        request_free(r);
    }
}

/* --- public API --- */

/* Generic implementation keyed on the base context. socket_ext_size is the full per-socket ext size
 * (for SSL the caller has already added the us_internal_ssl_socket_t overlay). */
struct us_connect_request_t *us_internal_socket_context_connect(struct us_socket_context_t *context,
    const struct us_connect_options_t *options, int socket_ext_size, int is_ssl) {

    /* Immediate argument failure -> return NULL, allocate nothing, fire nothing. */
    if (!context || !options || !options->host || options->port <= 0 || options->port > 65535) {
        return 0;
    }

    struct us_connect_request_t *r = (struct us_connect_request_t *) calloc(1, sizeof(*r));
    if (!r) {
        return 0;
    }

    r->loop = context->loop;
    r->socket_ext_size = socket_ext_size;
    r->handshake_timeout_ms = options->handshake_timeout_ms;
    r->client_verify = options->client_verify;
    r->is_ssl = is_ssl;
    r->state = US_CONNECT_RESOLVING;
    r->source_host = options->source_host ? strdup(options->source_host) : 0;
    {
        /* sni defaults to host; harmless for non-SSL (freed with the request in step 4). */
        const char *sni = options->sni_hostname ? options->sni_hostname : options->host;
        r->sni_hostname = sni ? strdup(sni) : 0;
    }

    link_request(context, r);

    /* One ms timer covers DNS + connect (armed now when timeout_ms > 0) and is later re-armed for the
     * handshake phase. Create it up front if either phase needs a deadline. */
    if (options->timeout_ms > 0 || options->handshake_timeout_ms > 0) {
        r->timer = us_create_timer(context->loop, 0, sizeof(struct us_connect_request_t *));
        *((struct us_connect_request_t **) us_timer_ext(r->timer)) = r;
        if (options->timeout_ms > 0) {
            us_timer_set(r->timer, connect_timer_cb, options->timeout_ms, 0);
        }
    }

    r->gai.data = r;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[16];
    snprintf(port_str, sizeof(port_str), "%d", options->port);

    int rc = uv_getaddrinfo(context->loop->uv_loop, &r->gai, connect_gai_cb,
        options->host, port_str, &hints);
    if (rc) {
        /* Synchronous resolver setup failure (rare: ENOMEM / bad args). No async callback will fire, so
         * clean up and report immediate failure. */
        request_close_timer(r);
        unlink_request(r);
        request_free(r);
        return 0;
    }

    context->loop->data.pending_resolves++;

    return r;
}

struct us_connect_request_t *us_socket_context_connect(int ssl, struct us_socket_context_t *context,
    const struct us_connect_options_t *options, int socket_ext_size) {
#ifndef LIBUS_NO_SSL
    if (ssl) {
        /* SSL connects route through the openssl.c wrapper, which adds the us_internal_ssl_socket_t
         * overlay delta to socket_ext_size (so the promotion memset zeroes the overlay's ssl and
         * client_sni) and then calls us_internal_socket_context_connect(..., 1). */
        return us_internal_ssl_socket_context_connect(
            (struct us_internal_ssl_socket_context_t *) context, options, socket_ext_size);
    }
#endif
    return us_internal_socket_context_connect(context, options, socket_ext_size, 0);
}

void us_connect_request_cancel(int ssl, struct us_connect_request_t *r) {
    (void) ssl; /* dispatch uses the request's own is_ssl */
    if (!r) {
        return;
    }

    request_close_timer(r);
    if (r->attempt) {
        us_socket_close_connecting(r->is_ssl, r->attempt);
        r->attempt = 0;
    }

    if (r->state == US_CONNECT_RESOLVING) {
        /* uv_getaddrinfo still in flight: unlink, mark CANCELLED and cancel. The (deferred) callback
         * reaps r; no on_connect_error fires. */
        r->state = US_CONNECT_CANCELLED;
        unlink_request(r);
        uv_cancel((uv_req_t *) &r->gai);
        return;
    }

    r->state = US_CONNECT_CANCELLED;
    request_drop_results(r);
    unlink_request(r);
    request_free(r);
}

void us_connect_request_rebind(struct us_connect_request_t *r, struct us_socket_t *s) {
    /* The socket was moved by us_socket_context_adopt_socket (realloc). Point at the new socket and
     * move the request from the old context's list onto the new one (s->context is the new context). */
    r->attempt = s;
    unlink_request(r);
    link_request(s->context, r);
}

void us_connect_request_release(struct us_connect_request_t *r) {
    if (!r) {
        return;
    }
    /* Called by the socket owner on frame-phase adoption or on any close before that. Does NOT touch
     * r->attempt: the owner now owns the socket. Safe to call from inside the handshake timer callback
     * (us_timer_close defers). */
    r->state = US_CONNECT_DONE;
    request_close_timer(r);
    request_drop_results(r);
    unlink_request(r);
    request_free(r);
}

/* Called from us_socket_context_free for each request still on the context list. */
void us_internal_connect_request_orphan(struct us_connect_request_t *r) {
    request_close_timer(r);

    if (r->state == US_CONNECT_RESOLVING) {
        /* uv_getaddrinfo still in flight: orphan and cancel. connect_gai_cb sees context == NULL on the
         * later loop turn and frees the request without dereferencing the freed context. */
        r->state = US_CONNECT_CANCELLED;
        r->context = NULL;
        uv_cancel((uv_req_t *) &r->gai);
        return;
    }

    /* No gai pending. Close a still-connecting attempt (a semi-socket); a promoted (HANDSHAKING) socket
     * is left to us_socket_context_close - such requests are normally already released via on_close
     * before we get here. Then free the request. */
    if (r->state == US_CONNECT_CONNECTING && r->attempt) {
        us_socket_close_connecting(r->is_ssl, r->attempt);
        r->attempt = 0;
    }
    request_drop_results(r);
    r->context = NULL;
    request_free(r);
}

#endif
