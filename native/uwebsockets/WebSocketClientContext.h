#ifndef UWS_WEBSOCKETCLIENTCONTEXT_H
#define UWS_WEBSOCKETCLIENTCONTEXT_H

/* Handshake-phase socket context for outbound WebSocket clients (spec A.3b). One of these owns a
 * connecting socket from on_open (write the GET upgrade request) until the 101 arrives, at which point
 * the socket is adopted into a WebSocketContext<SSL, false, USERDATA> for the frame phase - exactly the
 * inverse of the inbound HttpResponse::upgrade adoption.
 *
 * Scope note (step 8): the HTTP CONNECT proxy phase (HandshakePhase::PROXY_CONNECT) is now wired
 * alongside the direct ws:// / wss:// path. For a proxied wss:// connect the socket first opens on a
 * PLAINTEXT WebSocketClientContext<false> (the CONNECT phase runs unencrypted); after CONNECT 200 it is
 * adopted into the per-connect SSL WebSocketClientContext<true> (reached through the plaintext context's
 * sslPeer) and us_socket_start_tls() runs, after which it is indistinguishable from a direct wss://
 * socket. A proxied ws:// connect stays on its single plaintext context (the tunnel is a byte pipe). */

#include <cerrno>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "MoveOnlyFunction.h"
#include "AsyncSocketData.h"
#include "WebSocketClientHandshake.h"
#include "WebSocketContext.h"
#include "WebSocketContextData.h"
#include "WebSocketData.h"
#include "WebSocket.h"

namespace uWS {

enum class HandshakePhase : unsigned char {
    PROXY_CONNECT, /* step 8 only, when a proxy is configured */
    WS_UPGRADE
};

/* Delivered to behavior.failed when an outbound connection never reaches open (spec ConnectError). */
struct ClientConnectError {
    const char *code = "";   /* errno name, EAI_*, or HTTP_STATUS / HANDSHAKE_INVALID / TLS_* / ... */
    int status = 0;          /* HTTP status when code == HTTP_STATUS */
    int errnoCode = 0;       /* raw errno (>0) or libuv EAI_* (<0) for connect/DNS failures; 0 otherwise.
                              * Lets the addon layer refine `code` via uv_err_name without pulling libuv
                              * into µWS. */
    std::string_view message;
    /* Response headers (lower-cased) when code == HTTP_STATUS; owned by the caller for the call only. */
    const std::vector<std::pair<std::string, std::string>> *headers = nullptr;
};

/* Per-socket ext during the handshake. Survives adoption between handshake contexts by move (step 8),
 * so it carries everything the WS_UPGRADE phase needs plus the (defaulted) proxy fields for step 8. */
template <bool SSL>
struct HandshakeData : AsyncSocketData<SSL> {
    HandshakePhase phase = HandshakePhase::WS_UPGRADE;
    std::string proxyRequest;                 /* step 8: "CONNECT ..."; empty here */
    std::string request;                      /* the WS GET upgrade request, written at on_open */
    std::string response;                     /* accumulated until "\r\n\r\n" or the 8 KiB cap */
    std::string destSni;                      /* step 8: destination host for us_socket_start_tls */
    char key[24] = {};                        /* our Sec-WebSocket-Key, to verify the server's Accept */
    std::string protocols;                    /* offered subprotocols (CSV), to validate the choice */
    CompressOptions wanted = DISABLED;        /* from behavior.compression */
    void *user = nullptr;                     /* step 10: PerConnectData (JS userData + handlers) */
    us_connect_request_t *request_ = nullptr; /* owns the ms timer; released on adoption or close */
    bool wantsTls = false;                    /* step 8: start_tls after a proxy CONNECT 200 */
    int clientVerify = 0;                     /* per-socket TLS verify override for start_tls (proxied wss) */
};

/* Bookkeeping stored on the WebSocketClientContextData; moved into the socket's HandshakeData at
 * on_open. Because a handshake context is created per connect() (see the correlation note in ClientApp.h),
 * exactly one of these is live per context. */
template <bool SSL>
struct PendingConnect {
    std::string request;
    std::string proxyRequest;                 /* step 8 */
    std::string destSni;                      /* step 8 */
    char key[24] = {};
    std::string protocols;
    CompressOptions wanted = DISABLED;
    void *user = nullptr;
    us_connect_request_t *request_ = nullptr;
    bool wantsTls = false;                     /* step 8 */
    int clientVerify = 0;                      /* per-socket TLS verify override, carried to start_tls */
};

/* Context ext for the handshake context. */
template <bool SSL, typename USERDATA>
struct WebSocketClientContextData {
    /* The frame-phase context this connection adopts into once the 101 validates. */
    WebSocketContext<SSL, false, USERDATA> *frameContext = nullptr;

    /* Called at most once when the connection fails before open. */
    MoveOnlyFunction<void(ClientConnectError)> failedHandler = nullptr;

    /* The one pending connection this context is dialing. */
    PendingConnect<SSL> pending;

    /* Set at on_open; distinguishes an established-then-closed socket (HandshakeData built) from an
     * SSL socket whose TLS handshake failed before our on_open ever ran. */
    bool opened = false;

    /* Guards single delivery of failed across the detection site and on_close. */
    bool notified = false;

    /* Step 8 slot: the SSL handshake context a plaintext proxy socket adopts into after CONNECT 200. */
    us_socket_context_t *sslPeer = nullptr;

    /* Called at most once when this connection reaches a terminal FAILURE (never reached open). Lets the
     * owning TemplatedClientApp retire the per-connect slot so its contexts are freed on the next loop
     * tick, instead of leaking until the whole ClientApp is destroyed. Set by connect(); fired at the end
     * of emitFailed (inside the single-delivery guard, so it too runs exactly once). The success path
     * retires via the frame context's wrapped close handler instead. */
    MoveOnlyFunction<void()> onTerminal = nullptr;

    ~WebSocketClientContextData() {
        /* pending.user is nulled after consumption by emitFailed, adoptToFrame, proxyStartTls, and the
         * connect() NULL-request defer, so this only frees a connect cancelled before any of those paths. */
        if (pending.user) {
            delete (USERDATA *) pending.user;
        }
    }
    WebSocketClientContextData() {}
};

template <bool SSL, typename USERDATA>
struct WebSocketClientContext {
private:
    WebSocketClientContext() = delete;

    us_socket_context_t *getSocketContext() {
        return (us_socket_context_t *) this;
    }

    static WebSocketClientContextData<SSL, USERDATA> *getCtxData(us_socket_t *s) {
        return (WebSocketClientContextData<SSL, USERDATA> *) us_socket_context_ext(SSL, us_socket_context(SSL, s));
    }

    /* Map an errno / negative libuv EAI_* code (from us_socket_context_on_connect_error) to a name.
     * A fuller mapping (uv_err_name) belongs in the addon layer; this keeps the µWS layer uv-free. */
    static const char *errnoName(int code) {
        switch (code) {
            case ECONNREFUSED:  return "ECONNREFUSED";
            case ETIMEDOUT:     return "ETIMEDOUT";
            case ECONNRESET:    return "ECONNRESET";
            case ECONNABORTED:  return "ECONNABORTED";
            case EHOSTUNREACH:  return "EHOSTUNREACH";
            case ENETUNREACH:   return "ENETUNREACH";
            case ENETDOWN:      return "ENETDOWN";
            case EMFILE:        return "EMFILE";
            case ENFILE:        return "ENFILE";
            case ENOBUFS:       return "ENOBUFS";
            case ENOMEM:        return "ENOMEM";
            case EINVAL:        return "EINVAL";
            case EAFNOSUPPORT:  return "EAFNOSUPPORT";
            case EADDRNOTAVAIL: return "EADDRNOTAVAIL";
            case EADDRINUSE:    return "EADDRINUSE";
            case EACCES:        return "EACCES";
            default:
                /* Negative codes are libuv EAI_* resolver errors. */
                return code < 0 ? "EAI_FAIL" : "EUNKNOWN";
        }
    }

    /* Classify a pre-open close: TLS verify/handshake failure (reason string from openssl.c, step 5) or
     * a plain TCP reset. openssl.c closes with code 0 and a NUL-terminated C reason string (from
     * X509_verify_cert_error_string / ERR_reason_error_string), so when len is 0 but a reason is
     * present, treat it as NUL-terminated rather than discarding it. */
    static const char *closeCode(void *reason, size_t len) {
        if constexpr (SSL) {
            if (reason) {
                std::string_view r = len ? std::string_view((const char *) reason, len)
                                         : std::string_view((const char *) reason);
                if (r.find("verify") != std::string_view::npos ||
                    r.find("certificate") != std::string_view::npos) {
                    return "TLS_VERIFY";
                }
                return "TLS_HANDSHAKE";
            }
        }
        return "ECONNRESET";
    }

    static void emitFailed(WebSocketClientContextData<SSL, USERDATA> *ctxData, const char *code, int status,
            std::string_view message, const std::vector<std::pair<std::string, std::string>> *headers,
            int errnoCode = 0) {
        if (ctxData->notified) {
            return;
        }
        ctxData->notified = true;
        if (ctxData->failedHandler) {
            ClientConnectError err;
            err.code = code;
            err.status = status;
            err.errnoCode = errnoCode;
            err.message = message;
            err.headers = headers;
            ctxData->failedHandler(err);
        }
        /* Release the addon's per-connect userData (a heap USERDATA holding a V8 persistent). The
         * success path (adoptToFrame) moves it into the socket instead and nulls this pointer, so the
         * two paths are mutually exclusive and it is freed exactly once. USERDATA is complete wherever
         * WebSocketClientContext is instantiated (the addon's PerSocketData). */
        if (ctxData->pending.user) {
            delete (USERDATA *) ctxData->pending.user;
            ctxData->pending.user = nullptr;
        }
        /* Terminal: hand control back to the ClientApp so it can retire this connect's contexts. Deferred
         * inside onTerminal (a loop defer), so the handshake context is not freed under our own callback. */
        if (ctxData->onTerminal) {
            ctxData->onTerminal();
        }
    }

    /* Adopt the freshly-validated handshake socket into the frame context, mirror of HttpResponse::upgrade
     * (:295-341). Returns the (possibly relocated) socket. */
    static us_socket_t *adoptToFrame(us_socket_t *s, HandshakeData<SSL> *hd,
            WebSocketClientContextData<SSL, USERDATA> *ctxData, bool perMessageDeflate,
            CompressOptions compressOptions, std::string_view trailing) {

        WebSocketContext<SSL, false, USERDATA> *frameContext = ctxData->frameContext;
        auto *frameCtxData = (WebSocketContextData<SSL, false, USERDATA> *)
            us_socket_context_ext(SSL, (us_socket_context_t *) frameContext);

        /* Copy the bytes that trailed the header terminator before HandshakeData (and its response
         * buffer) are destroyed by the adoption. PerMessageDeflate::inflate temporarily writes a 4-byte
         * deflate trailer one past the end of its input (and restores it), relying on the loop recv
         * buffer's slack; the server path feeds recv_buf directly, but here the trailing frame bytes are
         * copied out, so pad the buffer with 4 bytes of real slack to keep that write in bounds. */
        size_t remainderLength = trailing.length();
        std::vector<char> remainder(remainderLength + 4);
        if (remainderLength) {
            memcpy(remainder.data(), trailing.data(), remainderLength);
        }

        /* The addon's per-connect userData, captured before HandshakeData is destroyed below. */
        void *user = hd->user;

        /* Release the connect request: closes the ms handshake timer. From here the frame context's
         * idleTimeout on the 4 s sweep is the only timer. */
        us_connect_request_release(hd->request_);
        hd->request_ = nullptr;

        /* Move the async socket state (backpressure buffer, cached remote address) out before adopting. */
        AsyncSocketData<SSL> asyncSocketData(std::move(*(AsyncSocketData<SSL> *) hd));

        /* Destroy HandshakeData - everything still needed has been copied/moved out. */
        hd->~HandshakeData();
        /* The socket is leaving this context; on_close on this context must not run again for it. */
        ctxData->opened = false;

        /* Adopting invalidates the old socket pointer. */
        WebSocket<SSL, false, USERDATA> *ws = (WebSocket<SSL, false, USERDATA> *) us_socket_context_adopt_socket(SSL,
            (us_socket_context_t *) frameContext, s, sizeof(WebSocketData<false>) + sizeof(USERDATA));

        /* init(): placement-new the frame-phase WebSocketData (identical to WebSocket::init). */
        new (us_socket_ext(SSL, (us_socket_t *) ws)) WebSocketData<false>(perMessageDeflate, compressOptions, std::move(asyncSocketData));

        /* Arm maxLifetime + idleTimeout from the frame context settings. */
        us_socket_long_timeout(SSL, (us_socket_t *) ws, frameCtxData->maxLifetime);
        us_socket_timeout(SSL, (us_socket_t *) ws, frameCtxData->idleTimeoutComponents.first);

        /* Move the addon's per-connect userData into the socket, mirroring the server upgrade path where
         * the userData persistent lives in the socket's USERDATA (::socketPf) at open. The heap USERDATA
         * is freed here after the move; the failure path frees it in emitFailed instead. */
        if (user) {
            new (ws->getUserData()) USERDATA(std::move(*(USERDATA *) user));
            delete (USERDATA *) user;
        } else {
            new (ws->getUserData()) USERDATA();
        }
        /* This connection has consumed its userData; ensure emitFailed (if ever reached) does not double
         * free. adoptToFrame and emitFailed are the only consumers and are mutually exclusive. */
        ctxData->pending.user = nullptr;

        /* Open event on the frame context (same contract as an inbound socket). */
        if (frameCtxData->openHandler) {
            frameCtxData->openHandler(ws);
        }

        /* Feed any bytes that arrived after the 101 terminator into the frame parser. */
        if (remainderLength && !us_socket_is_closed(SSL, (us_socket_t *) ws)) {
            WebSocketContext<SSL, false, USERDATA>::consume((us_socket_t *) ws, remainder.data(), (int) remainderLength);
        }

        return (us_socket_t *) ws;
    }

    /* CONNECT 200 on a PLAINTEXT proxy socket whose destination is wss:// (spec A.3b, "200 -> wantsTls").
     * Move the HandshakeData off the plaintext socket, adopt the socket into the per-connect SSL handshake
     * context (reached through sslPeer), placement-new the carried state there, rebind the connect request
     * onto the relocated socket + new context, start TLS toward the DESTINATION host, and write the (now
     * buffered) WS upgrade request. Only meaningful from a plaintext context; the SSL sibling is the target
     * of the adoption, so this is instantiated only for SSL == false (guarded by if constexpr at the call
     * site). The connect request is neither released nor re-created here - ownership simply moves with the
     * socket, so it is still released exactly once (by adoptToFrame on 101, or on_close on any failure). */
    static us_socket_t *proxyStartTls(us_socket_t *s, HandshakeData<false> *hd,
            WebSocketClientContextData<false, USERDATA> *plainCtxData) {

        us_socket_context_t *sslPeer = plainCtxData->sslPeer;
        auto *sslCtxData = (WebSocketClientContextData<true, USERDATA> *) us_socket_context_ext(1, sslPeer);

        /* Snapshot everything the SSL handshake phase still needs; the adopt realloc invalidates hd. The
         * write-backpressure buffer is carried as its raw string (BackPressure has a move ctor but no move
         * assignment, and in practice the tiny CONNECT request was fully flushed, so it is empty). */
        std::string request = std::move(hd->request);
        std::string destSni = std::move(hd->destSni);
        std::string protocols = std::move(hd->protocols);
        char key[24];
        memcpy(key, hd->key, sizeof(key));
        CompressOptions wanted = hd->wanted;
        void *user = hd->user;
        us_connect_request_t *request_ = hd->request_;
        std::string carriedWrite = std::move(hd->buffer.buffer);
        unsigned int carriedPending = hd->buffer.pendingRemoval;
        int savedEvents = hd->savedEvents;
        int clientVerify = hd->clientVerify;
#ifdef UWS_REMOTE_ADDRESS_USERSPACE
        char remoteAddress[sizeof(hd->remoteAddress)];
        memcpy(remoteAddress, hd->remoteAddress, sizeof(remoteAddress));
        int remoteAddressLength = hd->remoteAddressLength;
#endif

        /* From here on all failures surface on the SSL context, so hand it the failed callback. */
        sslCtxData->failedHandler = std::move(plainCtxData->failedHandler);

        /* Move ownership of the addon's per-connect userData to the SSL context. The plaintext context
         * no longer sees events for this socket (opened cleared below), so every subsequent free — SSL
         * failure (emitFailed on sslCtxData) or success (adoptToFrame) — goes through the SSL side. This
         * keeps the "freed exactly once" invariant across the plaintext -> SSL adoption. */
        sslCtxData->pending.user = user;
        plainCtxData->pending.user = nullptr;

        /* Destroy the plaintext HandshakeData and detach the socket from this (plaintext) context so its
         * on_close never runs again for this socket (request_ has already been captured above). */
        hd->~HandshakeData();
        plainCtxData->opened = false;

        /* Adopt the plaintext socket into the SSL handshake context. The SSL adopt reallocs with room for
         * the us_internal_ssl_socket_t overlay (start_tls precondition, spec A.3a); the returned pointer
         * replaces s. sizeof(HandshakeData<true>) == sizeof(HandshakeData<false>) (AsyncSocketData is
         * layout-identical across SSL), so the ext size is the same either way. */
        s = us_socket_context_adopt_socket(1, sslPeer, s, (int) sizeof(HandshakeData<true>));

        /* Placement-new the SSL-phase HandshakeData and restore the carried state. */
        HandshakeData<true> *nhd = new (us_socket_ext(1, s)) HandshakeData<true>();
        nhd->phase = HandshakePhase::WS_UPGRADE;
        nhd->request = std::move(request);
        nhd->destSni = std::move(destSni);
        memcpy(nhd->key, key, sizeof(key));
        nhd->protocols = std::move(protocols);
        nhd->wanted = wanted;
        nhd->user = user;
        nhd->request_ = request_;
        nhd->wantsTls = true;
        nhd->clientVerify = clientVerify;
        nhd->buffer.buffer = std::move(carriedWrite);
        nhd->buffer.pendingRemoval = carriedPending;
        nhd->savedEvents = savedEvents;
#ifdef UWS_REMOTE_ADDRESS_USERSPACE
        memcpy(nhd->remoteAddress, remoteAddress, sizeof(nhd->remoteAddress));
        nhd->remoteAddressLength = remoteAddressLength;
#endif

        /* The SSL handshake context now owns this socket's handshake lifetime. */
        sslCtxData->opened = true;

        /* Point the connect request (ms timer owner) at the relocated socket + new context request list. */
        us_connect_request_rebind(request_, s);

        /* Start TLS toward the DESTINATION host (SNI/verify), never the proxy host. From here ssl_on_data
         * drives the handshake exactly as for a direct wss:// socket. */
        us_socket_start_tls(s, nhd->destSni.c_str(), nhd->clientVerify);

        /* Write the WS upgrade request. During the TLS handshake SSL_write returns WANT_READ, so this is
         * fully buffered; the WANT_READ arms ssl_on_data to raise on_writable once the handshake completes,
         * which flushes nhd->buffer through the now-established TLS connection. */
        int len = (int) nhd->request.length();
        int written = us_socket_write(1, s, nhd->request.data(), len, 0);
        if (written < len) {
            nhd->buffer.append(nhd->request.data() + written, (size_t) (len - written));
        }
        return s;
    }

    WebSocketClientContext<SSL, USERDATA> *init() {
        /* on_open: build HandshakeData from the pending record and send the upgrade request. */
        us_socket_context_on_open(SSL, getSocketContext(), [](us_socket_t *s, int /*is_client*/, char *ip, int ip_length) {
            auto *ctxData = getCtxData(s);

            HandshakeData<SSL> *hd = new (us_socket_ext(SSL, s)) HandshakeData<SSL>();
#ifdef UWS_REMOTE_ADDRESS_USERSPACE
            /* Cache the peer address like HttpContext does for inbound sockets; it rides along into the
             * frame-phase WebSocketData at adoption. */
            if (ip_length > 0 && ip_length <= (int) sizeof(hd->remoteAddress)) {
                memcpy(hd->remoteAddress, ip, (size_t) ip_length);
                hd->remoteAddressLength = ip_length;
            }
#else
            (void) ip;
            (void) ip_length;
#endif
            hd->request = std::move(ctxData->pending.request);
            hd->proxyRequest = std::move(ctxData->pending.proxyRequest);
            hd->destSni = std::move(ctxData->pending.destSni);
            memcpy(hd->key, ctxData->pending.key, sizeof(hd->key));
            hd->protocols = std::move(ctxData->pending.protocols);
            hd->wanted = ctxData->pending.wanted;
            hd->user = ctxData->pending.user;
            hd->request_ = ctxData->pending.request_;
            hd->wantsTls = ctxData->pending.wantsTls;
            hd->clientVerify = ctxData->pending.clientVerify;
            ctxData->pending.request_ = nullptr;
            ctxData->opened = true;

            /* A configured proxy means the first thing on the wire is the CONNECT tunnel request; the WS
             * GET waits until the tunnel (and, for wss://, TLS) is up. No proxy => write the GET directly
             * (step 7 behaviour, unchanged for the direct path). */
            hd->phase = hd->proxyRequest.length() ? HandshakePhase::PROXY_CONNECT : HandshakePhase::WS_UPGRADE;
            std::string &toSend = (hd->phase == HandshakePhase::PROXY_CONNECT) ? hd->proxyRequest : hd->request;

            /* The ms timer was re-armed for the handshake phase by uSockets at promotion, so no
             * us_socket_timeout here. Write the request; a partial write is buffered for on_writable. */
            int len = (int) toSend.length();
            int written = us_socket_write(SSL, s, toSend.data(), len, 0);
            if (written < len) {
                hd->buffer.append(toSend.data() + written, (size_t) (len - written));
            }
            return s;
        });

        /* on_data: PROXY_CONNECT (proxy tunnel) then WS_UPGRADE (101 exchange). */
        us_socket_context_on_data(SSL, getSocketContext(), [](us_socket_t *s, char *data, int length) -> us_socket_t * {
            auto *ctxData = getCtxData(s);
            HandshakeData<SSL> *hd = (HandshakeData<SSL> *) us_socket_ext(SSL, s);

            /* --- PROXY_CONNECT: parse the proxy's CONNECT response (spec A.3b) --- */
            if (hd->phase == HandshakePhase::PROXY_CONNECT) {
                hd->response.append(data, (size_t) length);
                if (hd->response.length() > 8192) {
                    emitFailed(ctxData, "PROXY_STATUS", 0, "proxy response header too large", nullptr);
                    return us_socket_close(SSL, s, 0, nullptr);
                }

                WebSocketClientHandshake::ResponseHead head;
                if (!WebSocketClientHandshake::parseProxyResponse(hd->response.data(), hd->response.length(), head)) {
                    emitFailed(ctxData, "PROXY_STATUS", 0, "malformed proxy response", nullptr);
                    return us_socket_close(SSL, s, 0, nullptr);
                }
                if (!head.complete) {
                    return s; /* keep buffering up to the 8 KiB cap */
                }

                /* 407 -> the proxy demands authentication; surface Proxy-Authenticate in failed.headers. */
                if (head.status == 407) {
                    emitFailed(ctxData, "PROXY_AUTH", 407, "proxy authentication required", &head.headers);
                    return us_socket_close(SSL, s, 0, nullptr);
                }
                /* Any non-200, or bytes trailing a 200 (we have sent nothing into the tunnel yet, so
                 * anything past the terminator is a protocol violation) -> PROXY_STATUS. */
                if (head.status != 200 || hd->response.length() > head.headerBytes) {
                    emitFailed(ctxData, "PROXY_STATUS", head.status, "proxy CONNECT failed", &head.headers);
                    return us_socket_close(SSL, s, 0, nullptr);
                }

                /* 200: the tunnel is up. */
                if (hd->wantsTls) {
                    /* wss:// via proxy: adopt the plaintext socket into the SSL handshake context and start
                     * TLS. Only the plaintext (SSL == false) context ever carries wantsTls - a proxied
                     * wss:// connect always dials the plaintext sibling - so the SSL branch is unreachable. */
                    if constexpr (!SSL) {
                        return proxyStartTls(s, hd, ctxData);
                    }
                    return s;
                }

                /* ws:// via proxy: the tunnel is now a raw byte pipe; send the GET and fall into WS_UPGRADE
                 * on the next read. */
                hd->phase = HandshakePhase::WS_UPGRADE;
                hd->response.clear();
                int len = (int) hd->request.length();
                int written = us_socket_write(SSL, s, hd->request.data(), len, 0);
                if (written < len) {
                    hd->buffer.append(hd->request.data() + written, (size_t) (len - written));
                }
                return s;
            }

            /* --- WS_UPGRADE: accumulate, validate, adopt (step 7). --- */
            hd->response.append(data, (size_t) length);

            WebSocketClientHandshake::ResponseHead head;
            if (!WebSocketClientHandshake::parseResponse(hd->response.data(), hd->response.length(), head)) {
                emitFailed(ctxData, "HANDSHAKE_INVALID", 0, "malformed response", nullptr);
                return us_socket_close(SSL, s, 0, nullptr);
            }
            if (!head.complete) {
                /* The 8 KiB cap bounds only the HTTP header: a 101 may be immediately followed by a
                 * large data frame in the same read (legal, and consumed after adoption via the trailing
                 * bytes). Only an unterminated header that grows past the cap is an error. */
                if (hd->response.length() > 8192) {
                    emitFailed(ctxData, "HANDSHAKE_INVALID", 0, "response header too large", nullptr);
                    return us_socket_close(SSL, s, 0, nullptr);
                }
                return s; /* keep buffering */
            }

            /* Non-101: surface HTTP_STATUS with headers; no redirect following. */
            if (head.status != 101) {
                emitFailed(ctxData, "HTTP_STATUS", head.status, "unexpected HTTP status", &head.headers);
                return us_socket_close(SSL, s, 0, nullptr);
            }

            std::string_view upgrade = WebSocketClientHandshake::findHeader(head, "upgrade");
            std::string_view connection = WebSocketClientHandshake::findHeader(head, "connection");
            std::string_view accept = WebSocketClientHandshake::findHeader(head, "sec-websocket-accept");
            std::string_view protocol = WebSocketClientHandshake::findHeader(head, "sec-websocket-protocol");
            std::string_view extensions = WebSocketClientHandshake::findHeader(head, "sec-websocket-extensions");

            if (!WebSocketClientHandshake::hasToken(upgrade, "websocket") ||
                !WebSocketClientHandshake::hasToken(connection, "upgrade") ||
                !WebSocketClientHandshake::validateAccept(hd->key, accept) ||
                !WebSocketClientHandshake::validateProtocol(hd->protocols, protocol)) {
                emitFailed(ctxData, "HANDSHAKE_INVALID", 0, "invalid upgrade response", nullptr);
                return us_socket_close(SSL, s, 0, nullptr);
            }

            auto comp = WebSocketClientHandshake::acceptCompressionResponse(hd->wanted, extensions);
            if (!comp.ok) {
                emitFailed(ctxData, "HANDSHAKE_INVALID", 0, "invalid compression response", nullptr);
                return us_socket_close(SSL, s, 0, nullptr);
            }

            /* Success: adopt into the frame context and feed any trailing frame bytes. */
            std::string_view trailing(hd->response.data() + head.headerBytes, hd->response.length() - head.headerBytes);
            return adoptToFrame(s, hd, ctxData, comp.compression, comp.options, trailing);
        });

        /* on_connect_error: DNS / TCP connect failure. The request has already freed itself (spec A.3a). */
        us_socket_context_on_connect_error(SSL, getSocketContext(), [](us_socket_t *s, int code) {
            auto *ctxData = getCtxData(s);
            ctxData->pending.request_ = nullptr; /* freed itself */
            emitFailed(ctxData, errnoName(code), 0, "connect failed", nullptr, code);
            return s;
        });

        /* on_timeout: the ms handshake timer fired (never the 4 s sweep pre-adoption). */
        us_socket_context_on_timeout(SSL, getSocketContext(), [](us_socket_t *s) {
            auto *ctxData = getCtxData(s);
            emitFailed(ctxData, "ETIMEDOUT", 0, "handshake timed out", nullptr);
            return us_socket_close(SSL, s, 0, nullptr);
        });

        /* on_close before adoption: release the request exactly once, then failed (if not already). */
        us_socket_context_on_close(SSL, getSocketContext(), [](us_socket_t *s, int code, void *reason) {
            auto *ctxData = getCtxData(s);
            if (ctxData->opened) {
                HandshakeData<SSL> *hd = (HandshakeData<SSL> *) us_socket_ext(SSL, s);
                us_connect_request_release(hd->request_);
                hd->request_ = nullptr;
                if (!ctxData->notified) {
                    emitFailed(ctxData, closeCode(reason, (size_t) code), 0, "connection closed before open", nullptr);
                }
                hd->~HandshakeData();
                ctxData->opened = false;
            } else {
                /* SSL TLS failure before our on_open (no HandshakeData), or a plaintext pre-open close. */
                us_connect_request_release(ctxData->pending.request_);
                ctxData->pending.request_ = nullptr;
                if (!ctxData->notified) {
                    emitFailed(ctxData, closeCode(reason, (size_t) code), 0, "connection closed before open", nullptr);
                }
            }
            return s;
        });

        /* on_end: no half-close during a handshake, just close. */
        us_socket_context_on_end(SSL, getSocketContext(), [](us_socket_t *s) {
            return us_socket_close(SSL, s, 0, nullptr);
        });

        /* on_writable: flush any buffered request bytes (also the post-start_tls flush point in step 8). */
        us_socket_context_on_writable(SSL, getSocketContext(), [](us_socket_t *s) {
            auto *ctxData = getCtxData(s);
            if (!ctxData->opened || us_socket_is_shut_down(SSL, s)) {
                return s;
            }
            HandshakeData<SSL> *hd = (HandshakeData<SSL> *) us_socket_ext(SSL, s);
            if (hd->buffer.length()) {
                int w = us_socket_write(SSL, s, hd->buffer.data(), (int) hd->buffer.length(), 0);
                hd->buffer.erase((unsigned int) w);
            }
            return s;
        });

        return this;
    }

public:
    /* Created as a child of the ClientApp's parent context (so the SSL child shares the parent SSL_CTX,
     * matching how frame contexts child off their parent). */
    static WebSocketClientContext *create(us_socket_context_t *parentSocketContext) {
        WebSocketClientContext *context = (WebSocketClientContext *) us_create_child_socket_context(SSL,
            parentSocketContext, sizeof(WebSocketClientContextData<SSL, USERDATA>));
        if (!context) {
            return nullptr;
        }
        new (us_socket_context_ext(SSL, (us_socket_context_t *) context)) WebSocketClientContextData<SSL, USERDATA>();
        return context->init();
    }

    WebSocketClientContextData<SSL, USERDATA> *getExt() {
        return (WebSocketClientContextData<SSL, USERDATA> *) us_socket_context_ext(SSL, (us_socket_context_t *) this);
    }

    void free() {
        getExt()->~WebSocketClientContextData();
        us_socket_context_free(SSL, (us_socket_context_t *) this);
    }
};

}

#endif // UWS_WEBSOCKETCLIENTCONTEXT_H
