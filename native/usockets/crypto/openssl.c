#if (defined(LIBUS_USE_OPENSSL) || defined(LIBUS_USE_WOLFSSL))

/* These are in sni_tree.cpp */
void *sni_new();
void sni_free(void *sni, void(*cb)(void *));
int sni_add(void *sni, const char *hostname, void *user);
void *sni_remove(void *sni, const char *hostname);
void *sni_find(void *sni, const char *hostname);

#include "libusockets.h"
#include "internal/internal.h"
#include <stdio.h>
#include <string.h>

/* This module contains the entire OpenSSL implementation
 * of the SSL socket and socket context interfaces. */
#ifdef LIBUS_USE_OPENSSL
#include <openssl/ssl.h>
#include <openssl/bio.h>
#include <openssl/err.h>
#include <openssl/dh.h>
#include <openssl/x509.h>
#include <openssl/pem.h>
#elif LIBUS_USE_WOLFSSL
#include <wolfssl/options.h>
#include <wolfssl/openssl/ssl.h>
#include <wolfssl/openssl/bio.h>
#include <wolfssl/openssl/err.h>
#include <wolfssl/openssl/dh.h>
#endif

struct loop_ssl_data {
    char *ssl_read_input, *ssl_read_output;
    unsigned int ssl_read_input_length;
    unsigned int ssl_read_input_offset;
    struct us_socket_t *ssl_socket;
    struct us_socket_t *ssl_read_socket;
    struct us_internal_ssl_socket_t *pending_free_head;

    int last_write_was_msg_more;
    int msg_more;

    BIO *shared_rbio;
    BIO *shared_wbio;
    BIO_METHOD *shared_biom;
};

static int ssl_app_without_certificate_warned;

struct us_internal_ssl_socket_context_t {
    struct us_socket_context_t sc;

    // this thing can be shared with other socket contexts via socket transfer!
    // maybe instead of holding once you hold many, a vector or set
    // when a socket that belongs to another socket context transfers to a new socket context
    SSL_CTX *ssl_context;
    int is_parent;

    /* These decorate the base implementation */
    struct us_internal_ssl_socket_t *(*on_open)(struct us_internal_ssl_socket_t *, int is_client, char *ip, int ip_length);
    struct us_internal_ssl_socket_t *(*on_data)(struct us_internal_ssl_socket_t *, char *data, int length);
    struct us_internal_ssl_socket_t *(*on_writable)(struct us_internal_ssl_socket_t *);
    struct us_internal_ssl_socket_t *(*on_close)(struct us_internal_ssl_socket_t *, int code, void *reason);

    /* Called for missing SNI hostnames, if not NULL */
    void (*on_server_name)(struct us_internal_ssl_socket_context_t *, const char *hostname);

    /* Pointer to sni tree, created when the context is created and freed likewise when freed */
    void *sni;
};

// same here, should or shouldn't it contain s?
struct us_internal_ssl_socket_t {
    struct us_socket_t s;
    SSL *ssl;
    int ssl_write_wants_read; // we use this for now
    int ssl_read_wants_write;
    /* Client mode only: strdup'd SNI / hostname-verification name. Ownership moves in from the connect
     * request at promotion (connect.c) and is freed in ssl_on_close. Server sockets leave this NULL
     * (the promotion memset zeroes the whole overlay, so clients start NULL too, then get filled). */
    char *client_sni;
    /* Client mode only: per-socket TLS verify override. 0 = inherit the SSL_CTX verify mode, 1 = force
     * SSL_VERIFY_PEER, 2 = force SSL_VERIFY_NONE. Set from the connect options at promotion (connect.c)
     * or from us_socket_start_tls (proxied wss). 0 after the promotion memset, so the default inherits. */
    int client_verify;
    struct us_internal_ssl_socket_t *pending_free_next;
};

int passphrase_cb(char *buf, int size, int rwflag, void *u) {
    const char *passphrase = (const char *) u;
    int passphrase_length = (int) strlen(passphrase);
    if (passphrase_length > size) {
        passphrase_length = size;
    }
    memcpy(buf, passphrase, passphrase_length);
    // put null at end? no?
    return (int) passphrase_length;
}

int BIO_s_custom_create(BIO *bio) {
    BIO_set_init(bio, 1);
    return 1;
}

long BIO_s_custom_ctrl(BIO *bio, int cmd, long num, void *user) {
    switch(cmd) {
    case BIO_CTRL_FLUSH:
        return 1;
    default:
        return 0;
    }
}

int BIO_s_custom_write(BIO *bio, const char *data, int length) {
    struct loop_ssl_data *loop_ssl_data = (struct loop_ssl_data *) BIO_get_data(bio);

    // 16384 max TLS plaintext record + 29 bytes TLS 1.3 framing overhead.
    loop_ssl_data->last_write_was_msg_more = loop_ssl_data->msg_more || length == 16413;
    int written = us_socket_write(0, loop_ssl_data->ssl_socket, data, length, loop_ssl_data->last_write_was_msg_more);

    if (!written) {
        BIO_set_flags(bio, BIO_FLAGS_SHOULD_RETRY | BIO_FLAGS_WRITE);
        return -1;
    }

    return written;
}

int BIO_s_custom_read(BIO *bio, char *dst, int length) {
    struct loop_ssl_data *loop_ssl_data = (struct loop_ssl_data *) BIO_get_data(bio);

    if (!loop_ssl_data->ssl_read_input_length) {
        BIO_set_flags(bio, BIO_FLAGS_SHOULD_RETRY | BIO_FLAGS_READ);
        return -1;
    }

    if ((unsigned int) length > loop_ssl_data->ssl_read_input_length) {
        length = loop_ssl_data->ssl_read_input_length;
    }

    memcpy(dst, loop_ssl_data->ssl_read_input + loop_ssl_data->ssl_read_input_offset, length);

    loop_ssl_data->ssl_read_input_offset += length;
    loop_ssl_data->ssl_read_input_length -= length;
    return length;
}

/* Client-side pre-handshake wiring, shared by direct wss:// (ssl_on_open) and start_tls (step 6):
 * SNI, hostname verification and the fixed WebSocket ALPN. Only runs when sni is non-NULL; server
 * sockets never reach this. */
static void ssl_client_setup(struct us_internal_ssl_socket_t *s, const char *sni) {
    if (sni) {
        /* An IP literal must be verified against iPAddress SANs (set1_ip), not DNS SANs (set1_host would
         * always mismatch), and must NOT be offered as SNI - RFC 6066 forbids an IP in server_name and
         * some stacks stall the handshake on it. set1_ip_asc returns 1 only when sni parses as an IPv4/
         * IPv6 literal; otherwise it is a hostname and gets the usual SNI + DNS-name verification. */
        if (X509_VERIFY_PARAM_set1_ip_asc(SSL_get0_param(s->ssl), sni) != 1) {
            SSL_set_tlsext_host_name(s->ssl, sni);
            SSL_set1_host(s->ssl, sni);
        }
        SSL_set_alpn_protos(s->ssl, (const unsigned char *) "\x08http/1.1", 9);
    }
}

/* Apply a per-socket verify override (from the connect options / start_tls). 0 leaves the SSL_CTX
 * default in place; 1 forces verification on, 2 forces it off (self-signed dev servers). */
static void apply_client_verify(struct us_internal_ssl_socket_t *s) {
    if (s->client_verify == 1) {
        SSL_set_verify(s->ssl, SSL_VERIFY_PEER, NULL);
    } else if (s->client_verify == 2) {
        SSL_set_verify(s->ssl, SSL_VERIFY_NONE, NULL);
    }
}

struct us_internal_ssl_socket_t *ssl_on_open(struct us_internal_ssl_socket_t *s, int is_client, char *ip, int ip_length) {
    struct us_internal_ssl_socket_context_t *context = (struct us_internal_ssl_socket_context_t *) us_socket_context(0, &s->s);

    struct us_loop_t *loop = us_socket_context_loop(0, &context->sc);
    struct loop_ssl_data *loop_ssl_data = (struct loop_ssl_data *) loop->data.ssl_data;

    s->ssl = SSL_new(context->ssl_context);
    s->pending_free_next = 0;
    s->ssl_write_wants_read = 0;
    s->ssl_read_wants_write = 0;
    SSL_set_bio(s->ssl, loop_ssl_data->shared_rbio, loop_ssl_data->shared_wbio);

    BIO_up_ref(loop_ssl_data->shared_rbio);
    BIO_up_ref(loop_ssl_data->shared_wbio);

    if (is_client) {
        SSL_set_connect_state(s->ssl);
        /* client_sni and client_verify were moved into the ext by connect.c at promotion (NULL/0 for a
         * plain client). */
        ssl_client_setup(s, s->client_sni);
        apply_client_verify(s);
    } else {
        SSL_set_accept_state(s->ssl);
        /* Server sockets are created by the accept path, not connect.c's promotion memset, so the
         * client_sni ext byte is uninitialised here; NULL it so ssl_on_close never frees a garbage
         * pointer. (Client sockets have it set by connect.c before this runs.) */
        s->client_sni = NULL;
        s->client_verify = 0;
    }

    return (struct us_internal_ssl_socket_t *) context->on_open(s, is_client, ip, ip_length);
}

/* Start TLS on a socket opened PLAINTEXT and already adopted into an SSL context (step 6 / spec A.3a).
 * Precondition: the caller ran us_socket_context_adopt_socket(1, ssl_ctx, s, ext_size), so the ext has
 * room for the us_internal_ssl_socket_t overlay -- but that realloc copied stale plaintext bytes into the
 * overlay region, so s->ssl is garbage here. We do the same client-TLS setup ssl_on_open does for a
 * direct wss:// socket, only at call time instead of open time. */
struct us_socket_t *us_socket_start_tls(struct us_socket_t *s, const char *sni, int client_verify) {
    struct us_internal_ssl_socket_t *ssl_s = (struct us_internal_ssl_socket_t *) s;
    struct us_internal_ssl_socket_context_t *context = (struct us_internal_ssl_socket_context_t *) us_socket_context(0, s);

    struct us_loop_t *loop = us_socket_context_loop(0, &context->sc);
    struct loop_ssl_data *loop_ssl_data = (struct loop_ssl_data *) loop->data.ssl_data;

    /* Zero ONLY the overlay fields (never the base socket s->s). Safe precisely because no SSL* exists
     * yet -- unlike the generic SSL->SSL adopt, which relies on the copied ssl pointer surviving. */
    ssl_s->ssl = NULL;
    ssl_s->pending_free_next = 0;
    ssl_s->ssl_write_wants_read = 0;
    ssl_s->ssl_read_wants_write = 0;
    ssl_s->client_sni = NULL;
    ssl_s->client_verify = client_verify;

    /* SSL_new + BIO wiring, identical to ssl_on_open above (its SSL_new + SSL_set_bio + BIO_up_ref block). */
    ssl_s->ssl = SSL_new(context->ssl_context);
    SSL_set_bio(ssl_s->ssl, loop_ssl_data->shared_rbio, loop_ssl_data->shared_wbio);

    BIO_up_ref(loop_ssl_data->shared_rbio);
    BIO_up_ref(loop_ssl_data->shared_wbio);

    /* Client-side setup, matching ssl_on_open's is_client branch. */
    ssl_s->client_sni = sni ? strdup(sni) : NULL;
    SSL_set_connect_state(ssl_s->ssl);
    ssl_client_setup(ssl_s, ssl_s->client_sni);
    apply_client_verify(ssl_s);

    /* Kick off the ClientHello now instead of waiting for the first us_internal_ssl_socket_write. The
     * custom write BIO drains synchronously to the socket during the handshake (BIO_s_custom_write ->
     * us_socket_write), so point ssl_socket at us and clear msg_more first -- the same loop_ssl_data
     * setup us_internal_ssl_socket_write does before SSL_write. */
    loop_ssl_data->ssl_read_input_length = 0;
    struct us_socket_t *previous_ssl_read_socket = loop_ssl_data->ssl_read_socket;
    loop_ssl_data->ssl_read_socket = 0;
    loop_ssl_data->ssl_socket = &ssl_s->s;
    loop_ssl_data->msg_more = 0;
    loop_ssl_data->last_write_was_msg_more = 0;

    int ret = SSL_do_handshake(ssl_s->ssl);
    if (ret <= 0) {
        int err = SSL_get_error(ssl_s->ssl, ret);
        /* SSL_ERROR_WANT_READ is the expected result: the ClientHello has been written to the wbio (and
         * thus flushed to the socket) and we now wait for the server, whose reply ssl_on_data feeds back
         * through the read BIO to drive the rest of the handshake -- no new callback needed. */
        if (err == SSL_ERROR_SSL || err == SSL_ERROR_SYSCALL) {
            // these two errors may add to the per-thread error queue, which must be cleared
            ERR_clear_error();
        }
    }

    /* Mirror us_internal_ssl_socket_write's flush: if the BIO deferred the send as msg_more, push it. */
    if (loop_ssl_data->last_write_was_msg_more) {
        us_socket_flush(0, &ssl_s->s);
    }

    loop_ssl_data->ssl_read_socket = previous_ssl_read_socket;

    return s;
}

/* This one is a helper; it is entirely shared with non-SSL so can be removed */
struct us_internal_ssl_socket_t *us_internal_ssl_socket_close(struct us_internal_ssl_socket_t *s, int code, void *reason) {
    return (struct us_internal_ssl_socket_t *) us_socket_close(0, (struct us_socket_t *) s, code, reason);
}

struct us_internal_ssl_socket_t *ssl_on_close(struct us_internal_ssl_socket_t *s, int code, void *reason) {
    struct us_internal_ssl_socket_context_t *context = (struct us_internal_ssl_socket_context_t *) us_socket_context(0, &s->s);

    struct us_loop_t *loop = us_socket_context_loop(0, &context->sc);
    struct loop_ssl_data *loop_ssl_data = (struct loop_ssl_data *) loop->data.ssl_data;

    if (loop_ssl_data->ssl_read_socket == &s->s) {
        s->pending_free_next = loop_ssl_data->pending_free_head;
        loop_ssl_data->pending_free_head = s;
    } else {
        SSL_free(s->ssl);
        s->ssl = NULL;
    }

    /* Client sockets own the strdup'd SNI here (moved out of the connect request). free(NULL) makes this
     * a no-op for server sockets, whose client_sni is always NULL. */
    free(s->client_sni);

    return context->on_close(s, code, reason);
}

/* Move ownership of an strdup'd SNI/verification hostname into a freshly-promoted SSL client socket's
 * ext overlay. Called from connect.c at promotion so the us_internal_ssl_socket_t layout need not leak
 * into that translation unit. ssl_on_close frees it. */
void us_internal_ssl_socket_set_client_sni(struct us_socket_t *s, char *sni) {
    ((struct us_internal_ssl_socket_t *) s)->client_sni = sni;
}

/* Move a per-socket verify override into a freshly-promoted SSL client socket's ext (connect.c, at
 * promotion, alongside the SNI). 0 = inherit CTX default, 1 = force verify, 2 = force no-verify. */
void us_internal_ssl_socket_set_client_verify(struct us_socket_t *s, int client_verify) {
    ((struct us_internal_ssl_socket_t *) s)->client_verify = client_verify;
}

struct us_internal_ssl_socket_t *ssl_on_end(struct us_internal_ssl_socket_t *s) {
    // struct us_internal_ssl_socket_context_t *context = (struct us_internal_ssl_socket_context_t *) us_socket_context(0, &s->s);

    // whatever state we are in, a TCP FIN is always an answered shutdown

    /* Todo: this should report CLEANLY SHUTDOWN as reason */
    return us_internal_ssl_socket_close(s, 0, NULL);
}

// this whole function needs a complete clean-up
struct us_internal_ssl_socket_t *ssl_on_data(struct us_internal_ssl_socket_t *s, void *data, int length) {
    // note: this context can change when we adopt the socket!
    struct us_internal_ssl_socket_context_t *context = (struct us_internal_ssl_socket_context_t *) us_socket_context(0, &s->s);

    struct us_loop_t *loop = us_socket_context_loop(0, &context->sc);
    struct loop_ssl_data *loop_ssl_data = (struct loop_ssl_data *) loop->data.ssl_data;
    struct us_socket_t *previous_ssl_read_socket = loop_ssl_data->ssl_read_socket;
    loop_ssl_data->ssl_read_socket = &s->s;

    // note: if we put data here we should never really clear it (not in write either, it still should be available for SSL_write to read from!)
    loop_ssl_data->ssl_read_input = data;
    loop_ssl_data->ssl_read_input_length = length;
    loop_ssl_data->ssl_read_input_offset = 0;
    loop_ssl_data->ssl_socket = &s->s;
    loop_ssl_data->msg_more = 0;

    if (us_internal_ssl_socket_is_shut_down(s)) {

        int ret;
        if ((ret = SSL_shutdown(s->ssl)) == 1) {
            // two phase shutdown is complete here
            /* Todo: this should also report some kind of clean shutdown */
            loop_ssl_data->ssl_read_socket = previous_ssl_read_socket;
            return us_internal_ssl_socket_close(s, 0, NULL);
        } else if (ret < 0) {

            int err = SSL_get_error(s->ssl, ret);

            if (err == SSL_ERROR_SSL || err == SSL_ERROR_SYSCALL) {
                // we need to clear the error queue in case these added to the thread local queue
                ERR_clear_error();
            }

        }

        // no further processing of data when in shutdown state
        loop_ssl_data->ssl_read_socket = previous_ssl_read_socket;
        return s;
    }

    // bug checking: this loop needs a lot of attention and clean-ups and check-ups
    int read = 0;
    restart:
    while (1) {
        int just_read = SSL_read(s->ssl, loop_ssl_data->ssl_read_output + LIBUS_RECV_BUFFER_PADDING + read, LIBUS_RECV_BUFFER_LENGTH - read);

        if (just_read <= 0) {
            int err = SSL_get_error(s->ssl, just_read);

            // as far as I know these are the only errors we want to handle
            if (err != SSL_ERROR_WANT_READ && err != SSL_ERROR_WANT_WRITE) {

                /* Capture a reason string for the client failed event (step 7 maps it to TLS_VERIFY /
                 * TLS_HANDSHAKE) before we clear the error queue. Points into OpenSSL/BoringSSL static
                 * tables, so it stays valid for the synchronous on_close below. NULL for server sockets
                 * in practice (they close via other paths), preserving upstream behaviour. */
                const char *reason = NULL;

                // clear per thread error queue if it may contain something
                if (err == SSL_ERROR_SSL || err == SSL_ERROR_SYSCALL) {
                    unsigned long ssl_err = ERR_peek_error();
                    long verify_result = SSL_get_verify_result(s->ssl);
                    if (ERR_GET_REASON(ssl_err) == SSL_R_CERTIFICATE_VERIFY_FAILED && verify_result != X509_V_OK) {
                        /* A genuine certificate-verification failure (bad chain, expired, hostname/IP mismatch). The bare
                         * X509 strings ("Hostname mismatch", ...) do not all contain a keyword the µWS layer maps, so
                         * prefix a stable "verify:" marker -> TLS_VERIFY. The static buffer is consumed synchronously by
                         * the on_close below (single-threaded), before any other TLS op. */
                        static _Thread_local char verify_reason[256];
                        snprintf(verify_reason, sizeof(verify_reason), "verify: %s", X509_verify_cert_error_string(verify_result));
                        reason = verify_reason;
                    } else {
                        /* Any other TLS-layer failure (wrong version, unexpected message, plaintext response, ...) keeps
                         * its BoringSSL reason string and maps to TLS_HANDSHAKE. */
                        reason = ERR_reason_error_string(ssl_err);
                    }
                    ERR_clear_error();
                }

                // terminate connection here
                loop_ssl_data->ssl_read_socket = previous_ssl_read_socket;
                return us_internal_ssl_socket_close(s, 0, (void *) reason);
            } else {
                // emit the data we have and exit

                if (err == SSL_ERROR_WANT_WRITE) {
                    // here we need to trigger writable event next ssl_read!
                    s->ssl_read_wants_write = 1;
                }

                // assume we emptied the input buffer fully or error here as well!
                if (loop_ssl_data->ssl_read_input_length) {
                    loop_ssl_data->ssl_read_socket = previous_ssl_read_socket;
                    return us_internal_ssl_socket_close(s, 0, NULL);
                }

                // cannot emit zero length to app
                if (!read) {
                    break;
                }

                context = (struct us_internal_ssl_socket_context_t *) us_socket_context(0, &s->s);

                s = context->on_data(s, loop_ssl_data->ssl_read_output + LIBUS_RECV_BUFFER_PADDING, read);
                if (us_socket_is_closed(0, &s->s)) {
                    loop_ssl_data->ssl_read_socket = previous_ssl_read_socket;
                    return s;
                }

                break;
            }

        }

        read += just_read;

        // at this point we might be full and need to emit the data to application and start over
        if (read == LIBUS_RECV_BUFFER_LENGTH) {

            context = (struct us_internal_ssl_socket_context_t *) us_socket_context(0, &s->s);

            // emit data and restart
            s = context->on_data(s, loop_ssl_data->ssl_read_output + LIBUS_RECV_BUFFER_PADDING, read);
            if (us_socket_is_closed(0, &s->s)) {
                loop_ssl_data->ssl_read_socket = previous_ssl_read_socket;
                return s;
            }

            read = 0;
            goto restart;
        }
    }

    // trigger writable if we failed last write with want read
    if (s->ssl_write_wants_read) {
        s->ssl_write_wants_read = 0;

        // make sure to update context before we call (context can change if the user adopts the socket!)
        context = (struct us_internal_ssl_socket_context_t *) us_socket_context(0, &s->s);

        s = (struct us_internal_ssl_socket_t *) context->sc.on_writable(&s->s); // cast here!
        // if we are closed here, then exit
        if (us_socket_is_closed(0, &s->s)) {
            loop_ssl_data->ssl_read_socket = previous_ssl_read_socket;
            return s;
        }
    }

    // check this then?
    if (SSL_get_shutdown(s->ssl) & SSL_RECEIVED_SHUTDOWN) {
        //exit(-2);

        // not correct anyways!
        s = us_internal_ssl_socket_close(s, 0, NULL);

        //us_
    }

    loop_ssl_data->ssl_read_socket = previous_ssl_read_socket;
    return s;
}

struct us_internal_ssl_socket_t *ssl_on_writable(struct us_internal_ssl_socket_t *s) {

    struct us_internal_ssl_socket_context_t *context = (struct us_internal_ssl_socket_context_t *) us_socket_context(0, &s->s);

    // todo: cork here so that we efficiently output both from reading and from writing?

    if (s->ssl_read_wants_write) {
        s->ssl_read_wants_write = 0;

        // make sure to update context before we call (context can change if the user adopts the socket!)
        context = (struct us_internal_ssl_socket_context_t *) us_socket_context(0, &s->s);

        // if this one fails to write data, it sets ssl_read_wants_write again
        s = (struct us_internal_ssl_socket_t *) context->sc.on_data(&s->s, 0, 0); // cast here!
    }

    // should this one come before we have read? should it come always? spurious on_writable is okay
    s = context->on_writable(s);

    return s;
}

/* Lazily inits loop ssl data first time */
void us_internal_init_loop_ssl_data(struct us_loop_t *loop) {
    if (!loop->data.ssl_data) {
        struct loop_ssl_data *loop_ssl_data = malloc(sizeof(struct loop_ssl_data));

        loop_ssl_data->ssl_read_output = malloc(LIBUS_RECV_BUFFER_LENGTH + LIBUS_RECV_BUFFER_PADDING * 2);
        loop_ssl_data->ssl_read_socket = 0;
        loop_ssl_data->pending_free_head = 0;

        OPENSSL_init_ssl(0, NULL);

        loop_ssl_data->shared_biom = BIO_meth_new(BIO_TYPE_MEM, "µS BIO");
        BIO_meth_set_create(loop_ssl_data->shared_biom, BIO_s_custom_create);
        BIO_meth_set_write(loop_ssl_data->shared_biom, BIO_s_custom_write);
        BIO_meth_set_read(loop_ssl_data->shared_biom, BIO_s_custom_read);
        BIO_meth_set_ctrl(loop_ssl_data->shared_biom, BIO_s_custom_ctrl);

        loop_ssl_data->shared_rbio = BIO_new(loop_ssl_data->shared_biom);
        loop_ssl_data->shared_wbio = BIO_new(loop_ssl_data->shared_biom);
        BIO_set_data(loop_ssl_data->shared_rbio, loop_ssl_data);
        BIO_set_data(loop_ssl_data->shared_wbio, loop_ssl_data);

        loop->data.ssl_data = loop_ssl_data;
    }
}

void us_internal_ssl_free_pending(struct us_loop_t *loop) {
    if (!loop->data.ssl_data) {
        return;
    }

    struct loop_ssl_data *loop_ssl_data = (struct loop_ssl_data *) loop->data.ssl_data;
    struct us_internal_ssl_socket_t *s = loop_ssl_data->pending_free_head;
    while (s) {
        SSL_free(s->ssl);
        s->ssl = NULL;
        s = s->pending_free_next;
    }
    loop_ssl_data->pending_free_head = 0;
}

/* Called by loop free, clears any loop ssl data */
void us_internal_free_loop_ssl_data(struct us_loop_t *loop) {
    us_internal_ssl_free_pending(loop);

    struct loop_ssl_data *loop_ssl_data = (struct loop_ssl_data *) loop->data.ssl_data;

    if (loop_ssl_data) {
        free(loop_ssl_data->ssl_read_output);

        BIO_free(loop_ssl_data->shared_rbio);
        BIO_free(loop_ssl_data->shared_wbio);

        BIO_meth_free(loop_ssl_data->shared_biom);

        free(loop_ssl_data);
    }
}

// we throttle reading data for ssl sockets that are in init state. here we actually use
// the kernel buffering to our advantage
int ssl_is_low_prio(struct us_internal_ssl_socket_t *s) {
    /* SSL_in_init is true throughout the handshake, which is the CPU intensive phase we deprioritise so that
     * fully established connections increase linearly over time under high load */
    return SSL_in_init(s->ssl);
}

/* Per-context functions */
void *us_internal_ssl_socket_context_get_native_handle(struct us_internal_ssl_socket_context_t *context) {
    return context->ssl_context;
}

struct us_internal_ssl_socket_context_t *us_internal_create_child_ssl_socket_context(struct us_internal_ssl_socket_context_t *context, int context_ext_size) {
    /* Create a new non-SSL context */
    struct us_socket_context_options_t options = {0};
    struct us_internal_ssl_socket_context_t *child_context = (struct us_internal_ssl_socket_context_t *) us_create_socket_context(0, context->sc.loop, sizeof(struct us_internal_ssl_socket_context_t) - sizeof(struct us_socket_context_t) + context_ext_size, options);
    if (!child_context) {
        return NULL;
    }

    /* The only thing we share is SSL_CTX */
    child_context->ssl_context = context->ssl_context;
    child_context->is_parent = 0;

    return child_context;
}

/* Common function for creating a context from options.
 * We must NOT free a SSL_CTX with only SSL_CTX_free! Also free any password */
void free_ssl_context(SSL_CTX *ssl_context) {
    if (!ssl_context) {
        return;
    }

    /* If we have set a password string, free it here */
    void *password = SSL_CTX_get_default_passwd_cb_userdata(ssl_context);
    /* OpenSSL returns NULL if we have no set password */
    free(password);

    SSL_CTX_free(ssl_context);
}

/* This function should take any options and return SSL_CTX - which has to be free'd with
 * our destructor function - free_ssl_context() */
SSL_CTX *create_ssl_context_from_options(struct us_socket_context_options_t options) {
    /* Create the context */
    SSL_CTX *ssl_context = SSL_CTX_new(TLS_method());
    if (!ssl_context) {
        return NULL;
    }

    /* Default options we rely on - changing these will break our logic */
    SSL_CTX_set_read_ahead(ssl_context, 1);
    SSL_CTX_set_mode(ssl_context, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER);

    /* Anything below TLS 1.2 is disabled */
    SSL_CTX_set_min_proto_version(ssl_context, TLS1_2_VERSION);

    /* The following are helpers. You may easily implement whatever you want by using the native handle directly */

    /* Important option for lowering memory usage, but lowers performance slightly */
    if (options.ssl_prefer_low_memory_usage) {
       SSL_CTX_set_mode(ssl_context, SSL_MODE_RELEASE_BUFFERS);
    }

    if (options.passphrase) {
        /* When freeing the CTX we need to check SSL_CTX_get_default_passwd_cb_userdata and
         * free it if set */
        SSL_CTX_set_default_passwd_cb_userdata(ssl_context, (void *) strdup(options.passphrase));
        SSL_CTX_set_default_passwd_cb(ssl_context, passphrase_cb);
    }

    /* This one most probably do not need the cert_file_name string to be kept alive */
    if (options.cert_file_name) {
        if (SSL_CTX_use_certificate_chain_file(ssl_context, options.cert_file_name) != 1) {
            free_ssl_context(ssl_context);
            return NULL;
        }
    }

    /* Same as above - we can discard this string afterwards I suppose */
    if (options.key_file_name) {
        if (SSL_CTX_use_PrivateKey_file(ssl_context, options.key_file_name, SSL_FILETYPE_PEM) != 1) {
            free_ssl_context(ssl_context);
            return NULL;
        }
    }

    if (options.client_mode) {
        /* Client context: CA sources feed peer verification only. No client CA list is sent and no DH
         * parameters are set (both are server-only). cert/key/passphrase/ciphers above already apply. */
        int have_ca = 0;

        if (options.ca_file_name) {
            if (SSL_CTX_load_verify_locations(ssl_context, options.ca_file_name, NULL) != 1) {
                free_ssl_context(ssl_context);
                return NULL;
            }
            have_ca = 1;
        }

        if (options.ca_pem) {
            BIO *bio = BIO_new_mem_buf(options.ca_pem, -1);
            if (!bio) {
                free_ssl_context(ssl_context);
                return NULL;
            }

            X509_STORE *store = SSL_CTX_get_cert_store(ssl_context);
            X509 *x509;
            while ((x509 = PEM_read_bio_X509(bio, NULL, NULL, NULL)) != NULL) {
                X509_STORE_add_cert(store, x509);
                X509_free(x509);
                have_ca = 1;
            }

            /* The loop always ends on an expected "no start line" / EOF error; clear it so it does not
             * leak into a later SSL_get_error on this thread. */
            ERR_clear_error();
            BIO_free(bio);
        }

        /* No explicit CA source but the caller still wants verification: fall back to BoringSSL's
         * compile-time default paths (often empty on Linux; the JS layer supplies ca_pem in practice). */
        if (!have_ca && options.reject_unauthorized) {
            SSL_CTX_set_default_verify_paths(ssl_context);
        }

        SSL_CTX_set_verify(ssl_context, options.reject_unauthorized ? SSL_VERIFY_PEER : SSL_VERIFY_NONE, NULL);
    } else {
        if (options.ca_file_name) {
            STACK_OF(X509_NAME) *ca_list;
            ca_list = SSL_load_client_CA_file(options.ca_file_name);
            if(ca_list == NULL) {
                free_ssl_context(ssl_context);
                return NULL;
            }
            SSL_CTX_set_client_CA_list(ssl_context, ca_list);
            if (SSL_CTX_load_verify_locations(ssl_context, options.ca_file_name, NULL) != 1) {
                free_ssl_context(ssl_context);
                return NULL;
            }
            SSL_CTX_set_verify(ssl_context, SSL_VERIFY_PEER, NULL);
        }

        if (options.dh_params_file_name) {
            /* Set up ephemeral DH parameters. */
            DH *dh_2048 = NULL;
            FILE *paramfile;
            paramfile = fopen(options.dh_params_file_name, "r");

            if (paramfile) {
                dh_2048 = PEM_read_DHparams(paramfile, NULL, NULL, NULL);
                fclose(paramfile);
            } else {
                free_ssl_context(ssl_context);
                return NULL;
            }

            if (dh_2048 == NULL) {
                free_ssl_context(ssl_context);
                return NULL;
            }

            const long set_tmp_dh = SSL_CTX_set_tmp_dh(ssl_context, dh_2048);
            DH_free(dh_2048);

            if (set_tmp_dh != 1) {
                free_ssl_context(ssl_context);
                return NULL;
            }

            /* OWASP Cipher String 'A+' (https://www.owasp.org/index.php/TLS_Cipher_String_Cheat_Sheet) */
            if (SSL_CTX_set_cipher_list(ssl_context, "DHE-RSA-AES256-GCM-SHA384:DHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-RSA-AES128-GCM-SHA256") != 1) {
                free_ssl_context(ssl_context);
                return NULL;
            }
        }
    }

    if (options.ssl_ciphers) {
        if (SSL_CTX_set_cipher_list(ssl_context, options.ssl_ciphers) != 1) {
            free_ssl_context(ssl_context);
            return NULL;
        }
    }

    /* This must be free'd with free_ssl_context, not SSL_CTX_free */
    return ssl_context;
}

/* Returns a servername's userdata if any */
void *us_internal_ssl_socket_context_find_server_name_userdata(struct us_internal_ssl_socket_context_t *context, const char *hostname_pattern) {
    /* We can use sni_find because looking up a "wildcard pattern" will match the exact literal "wildcard pattern" first,
     * before it matches by the very wildcard itself, so it works fine (exact match is the only thing we care for here) */
    SSL_CTX *ssl_context = sni_find(context->sni, hostname_pattern);

    if (ssl_context) {
        return SSL_CTX_get_ex_data(ssl_context, 0);
    }

    return 0;
}

/* Clear a servername's userdata without removing its SSL_CTX, as live SSL sockets can still hold it */
void us_internal_ssl_socket_context_clear_server_name_userdata(struct us_internal_ssl_socket_context_t *context, const char *hostname_pattern) {
    SSL_CTX *ssl_context = sni_find(context->sni, hostname_pattern);

    if (ssl_context) {
        SSL_CTX_set_ex_data(ssl_context, 0, NULL);
    }
}

/* Returns either nullptr or the previously set user data attached to this SSL's selected SNI context */
void *us_internal_ssl_socket_get_sni_userdata(struct us_internal_ssl_socket_t *s) {
    return SSL_CTX_get_ex_data(SSL_get_SSL_CTX(s->ssl), 0);
}

int us_internal_ssl_socket_context_add_server_name_with_status(struct us_internal_ssl_socket_context_t *context, const char *hostname_pattern, struct us_socket_context_options_t options, void *user) {

    /* Try and construct an SSL_CTX from options */
    SSL_CTX *ssl_context = create_ssl_context_from_options(options);

    if (!ssl_context) {
        return -1;
    }

    /* Attach the user data to this context */
    SSL_CTX_set_ex_data(ssl_context, 0, user);

    /* We do not want to hold any nullptr's in our SNI tree */
    if (sni_add(context->sni, hostname_pattern, ssl_context)) {
        /* If we already had that name, ignore */
        free_ssl_context(ssl_context);
        return 1;
    }

    return 0;
}

void us_internal_ssl_socket_context_add_server_name(struct us_internal_ssl_socket_context_t *context, const char *hostname_pattern, struct us_socket_context_options_t options, void *user) {
    us_internal_ssl_socket_context_add_server_name_with_status(context, hostname_pattern, options, user);
}

void us_internal_ssl_socket_context_on_server_name(struct us_internal_ssl_socket_context_t *context, void (*cb)(struct us_internal_ssl_socket_context_t *, const char *hostname)) {
    context->on_server_name = cb;
}

void us_internal_ssl_socket_context_remove_server_name(struct us_internal_ssl_socket_context_t *context, const char *hostname_pattern) {

    /* The same thing must happen for sni_free, that's why we have a callback */
    SSL_CTX *sni_node_ssl_context = (SSL_CTX *) sni_remove(context->sni, hostname_pattern);
    free_ssl_context(sni_node_ssl_context);
}

/* Returns NULL or SSL_CTX. May call missing server name callback */
SSL_CTX *resolve_context(struct us_internal_ssl_socket_context_t *context, const char *hostname) {

    /* Try once first */
    void *user = sni_find(context->sni, hostname);
    if (!user) {
        /* Emit missing hostname then try again */
        if (!context->on_server_name) {
            /* We have no callback registered, so fail */
            return NULL;
        }

        context->on_server_name(context, hostname);

        /* Last try */
        user = sni_find(context->sni, hostname);
    }

    return user;
}

// arg is context
int sni_cb(SSL *ssl, int *al, void *arg) {

    if (ssl) {
        const char *hostname = SSL_get_servername(ssl, TLSEXT_NAMETYPE_host_name);
        if (hostname && hostname[0]) {
            /* Try and resolve (match) required hostname with what we have registered */
            SSL_CTX *resolved_ssl_context = resolve_context((struct us_internal_ssl_socket_context_t *) arg, hostname);
            if (resolved_ssl_context) {
                SSL_set_SSL_CTX(ssl, resolved_ssl_context);
            } else {
                /* Call a blocking callback notifying of missing context */
            }

            struct us_internal_ssl_socket_context_t *context = (struct us_internal_ssl_socket_context_t *) arg;
            struct loop_ssl_data *loop_ssl_data = (struct loop_ssl_data *) context->sc.loop->data.ssl_data;
            struct us_internal_ssl_socket_t *s = (struct us_internal_ssl_socket_t *) loop_ssl_data->ssl_socket;
            if (s && s->ssl == ssl && us_socket_is_closed(0, &s->s)) {
                *al = SSL_AD_INTERNAL_ERROR;
                return SSL_TLSEXT_ERR_ALERT_FATAL;
            }

        }

        return SSL_TLSEXT_ERR_OK;
    }

    /* Can we even come here ever? */
    return SSL_TLSEXT_ERR_NOACK;
}

struct us_internal_ssl_socket_context_t *us_internal_create_ssl_socket_context(struct us_loop_t *loop, int context_ext_size, struct us_socket_context_options_t options) {
    /* If we haven't initialized the loop data yet, do so .
     * This is needed because loop data holds shared OpenSSL data and
     * the function is also responsible for initializing OpenSSL */
    us_internal_init_loop_ssl_data(loop);

    /* First of all we try and create the SSL context from options */
    SSL_CTX *ssl_context = create_ssl_context_from_options(options);
    if (!ssl_context) {
        /* We simply fail early if we cannot even create the OpenSSL context */
        return NULL;
    }

    /* Otherwise ee continue by creating a non-SSL context, but with larger ext to hold our SSL stuff */
    struct us_internal_ssl_socket_context_t *context = (struct us_internal_ssl_socket_context_t *) us_create_socket_context(0, loop, sizeof(struct us_internal_ssl_socket_context_t) - sizeof(struct us_socket_context_t) + context_ext_size, options);
    if (!context) {
        free_ssl_context(ssl_context);
        return NULL;
    }

    /* I guess this is the only optional callback */
    context->on_server_name = NULL;

    /* Then we extend its SSL parts */
    context->ssl_context = ssl_context;//create_ssl_context_from_options(options);
    context->is_parent = 1;

    /* We, as parent context, may ignore data */
    context->sc.is_low_prio = (int (*)(struct us_socket_t *)) ssl_is_low_prio;

    /* Parent contexts may use SNI - but only servers do. A client context must not install the
     * servername callback (it is a server-side hook and meaningless for outbound handshakes). */
    if (!options.client_mode) {
        SSL_CTX_set_tlsext_servername_callback(context->ssl_context, sni_cb);
        SSL_CTX_set_tlsext_servername_arg(context->ssl_context, context);
    }

    /* Also create the SNI tree */
    context->sni = sni_new();

    return context;
}

/* Our destructor for hostnames, used below */
void sni_hostname_destructor(void *user) {
    /* Some nodes hold null, so this one must ignore this case */
    free_ssl_context((SSL_CTX *) user);
}

void us_internal_ssl_socket_context_free(struct us_internal_ssl_socket_context_t *context) {
    /* If we are parent then we need to free our OpenSSL context */
    if (context->is_parent) {
        free_ssl_context(context->ssl_context);

        /* Here we need to register a temporary callback for all still-existing hostnames
         * and their contexts. Only parents have an SNI tree */
        sni_free(context->sni, sni_hostname_destructor);
    }

    us_socket_context_free(0, &context->sc);
}

struct us_listen_socket_t *us_internal_ssl_socket_context_listen(struct us_internal_ssl_socket_context_t *context, const char *host, int port, int options, int socket_ext_size) {
    if (!SSL_CTX_get0_certificate(context->ssl_context) && !ssl_app_without_certificate_warned++) {
        fprintf(stderr, "uWS: SSLApp is listening without a certificate; TLS handshakes will fail\n");
    }
    return us_socket_context_listen(0, &context->sc, host, port, options, sizeof(struct us_internal_ssl_socket_t) - sizeof(struct us_socket_t) + socket_ext_size);
}

struct us_listen_socket_t *us_internal_ssl_socket_context_listen_unix(struct us_internal_ssl_socket_context_t *context, const char *path, int options, int socket_ext_size) {
    if (!SSL_CTX_get0_certificate(context->ssl_context) && !ssl_app_without_certificate_warned++) {
        fprintf(stderr, "uWS: SSLApp is listening without a certificate; TLS handshakes will fail\n");
    }
    return us_socket_context_listen_unix(0, &context->sc, path, options, sizeof(struct us_internal_ssl_socket_t) - sizeof(struct us_socket_t) + socket_ext_size);
}

/* SSL connect wrapper: enlarge the per-socket ext by the us_internal_ssl_socket_t overlay delta (same
 * arithmetic as the listen wrapper above) so the promotion memset in connect.c zeroes ssl/client_sni,
 * then drive the generic connect state machine with is_ssl = 1. */
struct us_connect_request_t *us_internal_ssl_socket_context_connect(struct us_internal_ssl_socket_context_t *context,
    const struct us_connect_options_t *options, int socket_ext_size) {
    return us_internal_socket_context_connect(&context->sc, options,
        sizeof(struct us_internal_ssl_socket_t) - sizeof(struct us_socket_t) + socket_ext_size, 1);
}

struct us_internal_ssl_socket_t *us_internal_ssl_adopt_accepted_socket(struct us_internal_ssl_socket_context_t *context, LIBUS_SOCKET_DESCRIPTOR accepted_fd,
    unsigned int socket_ext_size, char *addr_ip, int addr_ip_length) {
    return (struct us_internal_ssl_socket_t *) us_adopt_accepted_socket(0, &context->sc, accepted_fd, sizeof(struct us_internal_ssl_socket_t) - sizeof(struct us_socket_t) + socket_ext_size, addr_ip, addr_ip_length);
}

void us_internal_ssl_socket_context_on_open(struct us_internal_ssl_socket_context_t *context, struct us_internal_ssl_socket_t *(*on_open)(struct us_internal_ssl_socket_t *s, int is_client, char *ip, int ip_length)) {
    us_socket_context_on_open(0, &context->sc, (struct us_socket_t *(*)(struct us_socket_t *, int, char *, int)) ssl_on_open);
    context->on_open = on_open;
}

void us_internal_ssl_socket_context_on_close(struct us_internal_ssl_socket_context_t *context, struct us_internal_ssl_socket_t *(*on_close)(struct us_internal_ssl_socket_t *s, int code, void *reason)) {
    us_socket_context_on_close(0, (struct us_socket_context_t *) context, (struct us_socket_t *(*)(struct us_socket_t *, int, void *)) ssl_on_close);
    context->on_close = on_close;
}

void us_internal_ssl_socket_context_on_data(struct us_internal_ssl_socket_context_t *context, struct us_internal_ssl_socket_t *(*on_data)(struct us_internal_ssl_socket_t *s, char *data, int length)) {
    us_socket_context_on_data(0, (struct us_socket_context_t *) context, (struct us_socket_t *(*)(struct us_socket_t *, char *, int)) ssl_on_data);
    context->on_data = on_data;
}

void us_internal_ssl_socket_context_on_writable(struct us_internal_ssl_socket_context_t *context, struct us_internal_ssl_socket_t *(*on_writable)(struct us_internal_ssl_socket_t *s)) {
    us_socket_context_on_writable(0, (struct us_socket_context_t *) context, (struct us_socket_t *(*)(struct us_socket_t *)) ssl_on_writable);
    context->on_writable = on_writable;
}

void us_internal_ssl_socket_context_on_timeout(struct us_internal_ssl_socket_context_t *context, struct us_internal_ssl_socket_t *(*on_timeout)(struct us_internal_ssl_socket_t *s)) {
    us_socket_context_on_timeout(0, (struct us_socket_context_t *) context, (struct us_socket_t *(*)(struct us_socket_t *)) on_timeout);
}

void us_internal_ssl_socket_context_on_long_timeout(struct us_internal_ssl_socket_context_t *context, struct us_internal_ssl_socket_t *(*on_long_timeout)(struct us_internal_ssl_socket_t *s)) {
    us_socket_context_on_long_timeout(0, (struct us_socket_context_t *) context, (struct us_socket_t *(*)(struct us_socket_t *)) on_long_timeout);
}

/* We do not really listen to passed FIN-handler, we entirely override it with our handler since SSL doesn't really have support for half-closed sockets */
void us_internal_ssl_socket_context_on_end(struct us_internal_ssl_socket_context_t *context, struct us_internal_ssl_socket_t *(*on_end)(struct us_internal_ssl_socket_t *)) {
    us_socket_context_on_end(0, (struct us_socket_context_t *) context, (struct us_socket_t *(*)(struct us_socket_t *)) ssl_on_end);
}

void us_internal_ssl_socket_context_on_connect_error(struct us_internal_ssl_socket_context_t *context, struct us_internal_ssl_socket_t *(*on_connect_error)(struct us_internal_ssl_socket_t *, int code)) {
    us_socket_context_on_connect_error(0, (struct us_socket_context_t *) context, (struct us_socket_t *(*)(struct us_socket_t *, int)) on_connect_error);
}

void *us_internal_ssl_socket_context_ext(struct us_internal_ssl_socket_context_t *context) {
    return context + 1;
}

/* Per socket functions */
void *us_internal_ssl_socket_get_native_handle(struct us_internal_ssl_socket_t *s) {
    return s->ssl;
}

int us_internal_ssl_socket_write(struct us_internal_ssl_socket_t *s, const char *data, int length, int msg_more) {
    if (us_socket_is_closed(0, &s->s) || us_internal_ssl_socket_is_shut_down(s)) {
        return 0;
    }

    struct us_internal_ssl_socket_context_t *context = (struct us_internal_ssl_socket_context_t *) us_socket_context(0, &s->s);

    struct us_loop_t *loop = us_socket_context_loop(0, &context->sc);
    struct loop_ssl_data *loop_ssl_data = (struct loop_ssl_data *) loop->data.ssl_data;

    if (loop_ssl_data->ssl_read_socket != &s->s) {
        loop_ssl_data->ssl_read_input_length = 0;
    }
    loop_ssl_data->ssl_socket = &s->s;
    loop_ssl_data->msg_more = msg_more;
    loop_ssl_data->last_write_was_msg_more = 0;
    int written = SSL_write(s->ssl, data, length);
    loop_ssl_data->msg_more = 0;

    if (loop_ssl_data->last_write_was_msg_more && !msg_more) {
        us_socket_flush(0, &s->s);
    }

    if (written > 0) {
        return written;
    } else {
        int err = SSL_get_error(s->ssl, written);
        if (err == SSL_ERROR_WANT_READ) {
            // here we need to trigger writable event next ssl_read!
            s->ssl_write_wants_read = 1;
        } else if (err == SSL_ERROR_SSL || err == SSL_ERROR_SYSCALL) {
            // these two errors may add to the error queue, which is per thread and must be cleared
            ERR_clear_error();

            // all errors here except for want write are critical and should not happen
        }

        return 0;
    }
}

void *us_internal_ssl_socket_ext(struct us_internal_ssl_socket_t *s) {
    return s + 1;
}

int us_internal_ssl_socket_is_shut_down(struct us_internal_ssl_socket_t *s) {
    return us_socket_is_shut_down(0, &s->s) || SSL_get_shutdown(s->ssl) & SSL_SENT_SHUTDOWN;
}

void us_internal_ssl_socket_shutdown(struct us_internal_ssl_socket_t *s) {
    if (!us_socket_is_closed(0, &s->s) && !us_internal_ssl_socket_is_shut_down(s)) {
        struct us_internal_ssl_socket_context_t *context = (struct us_internal_ssl_socket_context_t *) us_socket_context(0, &s->s);
        struct us_loop_t *loop = us_socket_context_loop(0, &context->sc);
        struct loop_ssl_data *loop_ssl_data = (struct loop_ssl_data *) loop->data.ssl_data;

        if (loop_ssl_data->ssl_read_socket != &s->s) {
            loop_ssl_data->ssl_read_input_length = 0;
        }

        loop_ssl_data->ssl_socket = &s->s;


        loop_ssl_data->msg_more = 0;

        // sets SSL_SENT_SHUTDOWN no matter what (not actually true if error!)
        int ret = SSL_shutdown(s->ssl);
        if (ret == 0) {
            ret = SSL_shutdown(s->ssl);
        }

        if (ret < 0) {

            int err = SSL_get_error(s->ssl, ret);
            if (err == SSL_ERROR_SSL || err == SSL_ERROR_SYSCALL) {
                // clear
                ERR_clear_error();
            }

            // we get here if we are shutting down while still in init
            us_socket_shutdown(0, &s->s);
        }
    }
}

struct us_internal_ssl_socket_t *us_internal_ssl_socket_context_adopt_socket(struct us_internal_ssl_socket_context_t *context, struct us_internal_ssl_socket_t *s, int ext_size) {
    // todo: this is completely untested
    return (struct us_internal_ssl_socket_t *) us_socket_context_adopt_socket(0, &context->sc, &s->s, sizeof(struct us_internal_ssl_socket_t) - sizeof(struct us_socket_t) + ext_size);
}

#endif
