#ifndef UWS_CLIENTAPP_H
#define UWS_CLIENTAPP_H

/* Owner object for the outbound WebSocket client role (spec A.3b "ClientApp.h"). Mirrors TemplatedApp:
 * a builder that operates on the thread-local Loop, owns a TopicTree isolated from any server App, and
 * a per-connect WebSocketContext<SSL, false, PerSocketData> recorded with a deleter exactly like App::ws.
 *
 * Correlation note (important deviation from the spec's "one handshake context per ClientApp"):
 * uSockets' connect path (connect.c) memsets the connecting socket's ext at promotion, before on_open,
 * and exposes no socket->request lookup. With a single shared handshake context, on_open therefore has
 * no way to tell WHICH pending connect a freshly-opened socket belongs to (concurrent connects resolve
 * DNS out of order). So each connect() creates its OWN handshake context as a CHILD of a single per-app
 * parent context. The parent owns the (client) SSL_CTX so the CA bundle is parsed once per ClientApp
 * (spec's cost concern), while the per-connect child makes on_open unambiguous. The frame context is
 * also a child of the same parent, so the SSL adoption from handshake->frame keeps one SSL_CTX. */

#include <charconv>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "App.h" /* SocketContextOptions, Loop, TopicTree, WebSocketContext, PerMessageDeflate */
#include "WebSocketClientContext.h"

namespace uWS {

/* Split proxy components consumed by connect() (spec A.3b). The step-8 CONNECT machinery takes these
 * already parsed; step 12's addon fills them from the proxy URL (userinfo -> base64 authHeader,
 * percent-decode, http-scheme/port validation). authHeader is the ready Proxy-Authorization value
 * (e.g. "Basic <base64>"), empty when the proxy URL had no userinfo. */
struct ProxyConfig {
    bool enabled = false;
    std::string host;
    int port = 0;
    std::string authHeader;
};

/* One per connect() call: the handshake + frame contexts it created, how to free each, and how to cancel
 * a still-pending connect request. Freed as a unit when the connection reaches a terminal state (failure
 * before open, or the frame-phase socket closing), which is what bounds native growth for a long-lived
 * reconnecting Client (spec production-fixes F1). The ConnectSlot object is heap-allocated behind a
 * unique_ptr so its address stays stable while socket callbacks hold a raw pointer to it across
 * reallocations of ClientState::slots. */
struct ConnectSlot {
    /* Raw us_socket_context_t* for every context this connect owns; close() closes each. Order is
     * frame, then handshake context(s) (plaintext before SSL on the proxied-wss path). */
    std::vector<void *> contexts;
    /* Type-erased frees, one per entry in contexts and in the same order. Run once, at retire/teardown. */
    std::vector<MoveOnlyFunction<void()>> deleters;
    /* Cancels a still-pending connect request (tolerates a null request_, so it is a no-op once opened). */
    MoveOnlyFunction<void()> cancel = nullptr;
    /* Set once the connection is terminal; the slot is then swept on the next loop tick. */
    bool done = false;
};

/* Per-ClientApp control block, owned by a shared_ptr. Holds the parent context (the client SSL_CTX owner)
 * and every live per-connect slot. A shared_ptr control block (rather than owning these directly on the
 * ClientApp) is what makes the deferred slot sweep safe: the sweep captures only a weak_ptr, so destroying
 * the ClientApp (e.g. the JS Client is GC'd) while a sweep is still queued turns the sweep into a no-op
 * instead of touching freed state. ~ClientState enforces close-before-free for every remaining slot, then
 * frees its contexts (handshake + frame children) BEFORE the parent, preserving the SSL_CTX-outlives-children
 * ordering. */
template <bool SSL>
struct ClientState : std::enable_shared_from_this<ClientState<SSL>> {
    us_socket_context_t *parentContext = nullptr;
    std::vector<std::unique_ptr<ConnectSlot>> slots;
    bool sweepScheduled = false;
    bool closed = false;

    ~ClientState() {
        /* Enforce close-before-free if the owner did not call close(). This is the same teardown as close():
         * cancel pending connects before closing each context, so no live socket can retain a freed context. */
        if (!closed) {
            closed = true;
            for (auto &slot : slots) {
                if (slot->cancel) {
                    slot->cancel();
                }
                for (void *c : slot->contexts) {
                    us_socket_context_close(SSL, (us_socket_context_t *) c);
                }
            }
        }

        /* Children first (their sockets have now been detached; freeing an empty child context never touches
         * the parent's SSL_CTX), then the parent. */
        for (auto &slot : slots) {
            for (auto &deleter : slot->deleters) {
                deleter();
            }
        }
        slots.clear();
        if (parentContext) {
            us_socket_context_free(SSL, parentContext);
        }
    }

    /* Drop every slot marked done, freeing its contexts. Runs on a loop tick, after the socket callbacks
     * that retired the slots have fully unwound, so us_socket_context_free never runs under its own poll. */
    void sweep() {
        sweepScheduled = false;
        for (size_t i = 0; i < slots.size();) {
            if (slots[i]->done) {
                for (auto &deleter : slots[i]->deleters) {
                    deleter();
                }
                slots.erase(slots.begin() + (long) i);
            } else {
                i++;
            }
        }
    }

    /* Mark a connect terminal and arm the sweep (idempotent per slot and per tick). */
    void retire(ConnectSlot *slot) {
        if (slot->done) {
            return;
        }
        slot->done = true;
        if (!sweepScheduled) {
            sweepScheduled = true;
            std::weak_ptr<ClientState<SSL>> weak = this->weak_from_this();
            Loop::get()->defer([weak]() {
                if (auto self = weak.lock()) {
                    self->sweep();
                }
            });
        }
    }
};

template <bool SSL>
struct TemplatedClientApp {
private:
    /* Per-app control block (parent context + live per-connect slots). shared so a deferred sweep can hold
     * a weak_ptr and safely become a no-op if this ClientApp is destroyed first. */
    std::shared_ptr<ClientState<SSL>> state;

    /* Percent-encoding is passed through untouched; only structural parsing happens here. */
    static bool parseUrl(std::string_view url, std::string &scheme, std::string &host,
            int &port, bool &defaultPort, std::string &target) {
        size_t schemeEnd = url.find("://");
        if (schemeEnd == std::string_view::npos) {
            return false;
        }
        scheme.assign(url.substr(0, schemeEnd));
        std::string_view rest = url.substr(schemeEnd + 3);

        /* Authority ends at the first '/' or '?'. */
        size_t authEnd = rest.find_first_of("/?");
        std::string_view authority = (authEnd == std::string_view::npos) ? rest : rest.substr(0, authEnd);
        std::string_view path = (authEnd == std::string_view::npos) ? std::string_view{} : rest.substr(authEnd);

        std::string_view portStr;
        if (authority.length() && authority.front() == '[') {
            /* Bracketed IPv6 literal. */
            size_t close = authority.find(']');
            if (close == std::string_view::npos) {
                return false;
            }
            host.assign(authority.substr(1, close - 1));
            std::string_view after = authority.substr(close + 1);
            if (after.length()) {
                if (after.front() != ':') {
                    return false;
                }
                portStr = after.substr(1);
            }
        } else {
            size_t colon = authority.rfind(':');
            if (colon == std::string_view::npos) {
                host.assign(authority);
            } else {
                host.assign(authority.substr(0, colon));
                portStr = authority.substr(colon + 1);
            }
        }
        if (!host.length()) {
            return false;
        }

        defaultPort = !portStr.length();
        if (defaultPort) {
            port = SSL ? 443 : 80;
        } else {
            int p = 0;
            auto r = std::from_chars(portStr.data(), portStr.data() + portStr.length(), p);
            if (r.ec != std::errc() || r.ptr != portStr.data() + portStr.length() || p <= 0 || p > 65535) {
                return false;
            }
            port = p;
        }

        target = path.length() ? std::string(path) : std::string("/");
        return true;
    }

    static bool hasControlOrCRLF(std::string_view value) {
        for (unsigned char c : value) {
            if (c < 0x20 || c == 0x7f) {
                return true;
            }
        }
        return false;
    }

    /* Lazily create the client's own TopicTree (never shared with a server App - client sockets are cast
     * to WebSocket<SSL, false, int> so they are always sent masked frames). Mirror of App::ws. */
    void ensureTopicTree() {
        if (topicTree) {
            return;
        }
        /* Mirror App::ws corking to batch each subscriber's drain. */
        bool needsUncork = false;
        topicTree = new TopicTree<TopicTreeMessage, TopicTreeBigMessage>([needsUncork](Subscriber *s, TopicTreeMessage &message, TopicTree<TopicTreeMessage, TopicTreeBigMessage>::IteratorFlags flags) mutable {
            auto *ws = (WebSocket<SSL, false, int> *) s->user;

            /* If this is the first message we try and cork */
            if (flags & TopicTree<TopicTreeMessage, TopicTreeBigMessage>::IteratorFlags::FIRST) {
                if (ws->canCork() && !ws->isCorked()) {
                    ((AsyncSocket<SSL> *)ws)->cork();
                    needsUncork = true;
                }
            }

            /* If we ever overstep maxBackpresure, exit immediately */
            if (WebSocket<SSL, false, int>::SendStatus::DROPPED == ws->send(message.message, (OpCode) message.opCode, message.compress)) {
                if (needsUncork) {
                    ((AsyncSocket<SSL> *)ws)->uncork();
                    needsUncork = false;
                }
                /* Stop draining */
                return true;
            }

            /* If this is the last message we uncork if we are corked */
            if (flags & TopicTree<TopicTreeMessage, TopicTreeBigMessage>::IteratorFlags::LAST) {
                /* We should not uncork in all cases? */
                if (needsUncork) {
                    ((AsyncSocket<SSL> *)ws)->uncork();
                }
            }

            /* Success */
            return false;
        });

        Loop::get()->addPostHandler(topicTree, [topicTree = topicTree](Loop *) {
            topicTree->drain();
        });
        Loop::get()->addPreHandler(topicTree, [topicTree = topicTree](Loop *) {
            topicTree->drain();
        });
    }

public:
    TopicTree<TopicTreeMessage, TopicTreeBigMessage> *topicTree = nullptr;

    /* Client-level default proxy (from ClientOptions.proxy), already parsed by the addon's proxy-URL
     * parser. Copied into a connect's ConnectBehavior.proxy when that connect specifies no proxy of its
     * own. Disabled (enabled == false) by default. */
    ProxyConfig defaultProxy;

    template <typename UserData>
    struct ConnectBehavior {
        /* Shared with WebSocketBehavior (frame-phase settings). */
        CompressOptions compression = DISABLED;
        unsigned int maxPayloadLength = 16 * 1024;
        unsigned short idleTimeout = 120;
        unsigned int maxBackpressure = 64 * 1024;
        bool closeOnBackpressureLimit = false;
        bool resetIdleTimeoutOnSend = false;
        bool sendPingsAutomatically = true;
        unsigned short maxLifetime = 0;

        /* Client-specific. */
        int connectTimeout = 10000;      /* ms for DNS + TCP connect */
        int handshakeTimeout = 10000;    /* ms allowed for the 101 exchange (matches connectTimeout units) */
        int rejectUnauthorized = -1;     /* -1 = inherit context; 0/1 override (SSLClient) */
        std::string servername;          /* SNI + verify host override */
        std::string sourceHost;          /* local bind */
        ProxyConfig proxy;               /* per-connect proxy (already split; step 12 fills from a URL) */
        std::vector<std::string> protocols;
        std::vector<std::pair<std::string, std::string>> headers;
        void *userData = nullptr;        /* step 10: PerConnectData */

        MoveOnlyFunction<void(WebSocket<SSL, false, UserData> *)> open = nullptr;
        MoveOnlyFunction<void(WebSocket<SSL, false, UserData> *, std::string_view, OpCode)> message = nullptr;
        MoveOnlyFunction<void(WebSocket<SSL, false, UserData> *, std::string_view, OpCode)> dropped = nullptr;
        MoveOnlyFunction<void(WebSocket<SSL, false, UserData> *)> drain = nullptr;
        MoveOnlyFunction<void(WebSocket<SSL, false, UserData> *, std::string_view)> ping = nullptr;
        MoveOnlyFunction<void(WebSocket<SSL, false, UserData> *, std::string_view)> pong = nullptr;
        MoveOnlyFunction<void(WebSocket<SSL, false, UserData> *, std::string_view, int, int)> subscription = nullptr;
        MoveOnlyFunction<void(WebSocket<SSL, false, UserData> *, int, std::string_view)> close = nullptr;
        MoveOnlyFunction<void(ClientConnectError)> failed = nullptr;
    };

    TemplatedClientApp(SocketContextOptions options = {}) {
        /* Force client-mode TLS semantics (CA-for-verify, SNI, no client-CA-list); parses the CA bundle
         * once for the lifetime of this ClientApp. */
        options.client_mode = 1;
        state = std::make_shared<ClientState<SSL>>();
        state->parentContext = us_create_socket_context(SSL, (us_loop_t *) Loop::get(), 0, options);
    }

    TemplatedClientApp(const TemplatedClientApp &) = delete;
    TemplatedClientApp &operator=(const TemplatedClientApp &) = delete;

    TemplatedClientApp(TemplatedClientApp &&other) {
        state = std::move(other.state);
        defaultProxy = std::move(other.defaultProxy);
        topicTree = other.topicTree;
        other.topicTree = nullptr;
    }

    ~TemplatedClientApp() {
        /* Dropping the last strong ref runs ~ClientState: it closes every remaining slot, then frees its
         * contexts (handshake + frame children) and finally the parent (SSL_CTX owner). A still-pending
         * deferred sweep holds only a weak_ptr, so it degrades to a no-op rather than touching freed state. */
        state.reset();

        if (topicTree) {
            Loop::get()->removePostHandler(topicTree);
            Loop::get()->removePreHandler(topicTree);
            delete topicTree;
        }
    }

    bool constructorFailed() {
        return !state || !state->parentContext;
    }

    /* Cancels pending connects and closes all open sockets. Closing a frame socket fires its (wrapped)
     * close handler, which retires the slot; a cancelled still-pending connect never opens, so its slot is
     * freed at destruction instead. */
    TemplatedClientApp &&close() {
        if (state) {
            state->closed = true;
            /* Cancel first: a still-resolving request would otherwise open a socket after close(). Iterate
             * by index because a synchronous on_close from us_socket_context_close only marks a slot done
             * (the actual erase is deferred to sweep()), so slots is never mutated under us here. */
            for (auto &slot : state->slots) {
                if (slot->cancel) {
                    slot->cancel();
                }
                for (void *c : slot->contexts) {
                    us_socket_context_close(SSL, (us_socket_context_t *) c);
                }
            }
        }
        return std::move(static_cast<TemplatedClientApp &&>(*this));
    }

    /* Publishes to all this client's sockets subscribed to topic. */
    bool publish(std::string_view topic, std::string_view message, OpCode opCode, bool compress = false) {
        if (!topicTree) {
            return false;
        }
        if (message.length() >= LoopData::CORK_BUFFER_SIZE && !topicTree->isDraining()) {
            return topicTree->publishBig(nullptr, topic, {message, opCode, compress}, [](Subscriber *s, TopicTreeBigMessage &message) {
                auto *ws = (WebSocket<SSL, false, int> *) s->user;
                ws->send(message.message, (OpCode) message.opCode, message.compress);
            });
        }
        return topicTree->publish(nullptr, topic, {std::string(message), opCode, compress});
    }

    unsigned int numSubscribers(std::string_view topic) {
        if (!topicTree) {
            return 0;
        }
        Topic *t = topicTree->lookupTopic(topic);
        return t ? (unsigned int) topicTree->numSubscribers(t) : 0;
    }

    /* Starts an outbound connection. Throws synchronously on an invalid URL, scheme mismatch or a bad
     * header; every other failure is delivered to behavior.failed. */
    template <typename UserData>
    TemplatedClientApp &&connect(std::string url, ConnectBehavior<UserData> behavior) {
        if (!state || !state->parentContext) {
            return std::move(static_cast<TemplatedClientApp &&>(*this));
        }
        /* Reuse after close() is not supported: a closed app has torn down (or is tearing down) its sockets
         * and must not start new ones (spec production-fixes F1/F8). */
        if (state->closed) {
            throw std::runtime_error("CLIENT_CLOSED");
        }

        /* --- parse + validate the URL --- */
        std::string scheme, host, target;
        int port = 0;
        bool defaultPort = false;
        if (!parseUrl(url, scheme, host, port, defaultPort, target)) {
            throw std::runtime_error("URL_INVALID");
        }
        if ((SSL && scheme != "wss") || (!SSL && scheme != "ws")) {
            throw std::runtime_error("URL_INVALID");
        }
        if (hasControlOrCRLF(host) || hasControlOrCRLF(target)) {
            throw std::runtime_error("URL_INVALID");
        }

        /* --- validate user headers (reject CR/LF + reserved names) --- */
        for (auto &h : behavior.headers) {
            if (!WebSocketClientHandshake::isValidUserHeader(h.first, h.second)) {
                throw std::runtime_error("HEADER_INVALID");
            }
        }

        /* --- clamp idleTimeout the same way App::ws does --- */
        if (behavior.idleTimeout && behavior.idleTimeout < 8) {
            behavior.idleTimeout = 8;
        }
        if (behavior.idleTimeout > 240 * 4) {
            behavior.idleTimeout = 240 * 4;
        }
        if (behavior.maxLifetime > 240) {
            behavior.maxLifetime = 240;
        }

#ifdef UWS_NO_ZLIB
        behavior.compression = DISABLED;
#endif

        ensureTopicTree();

        /* --- build the Sec-WebSocket-Key and the upgrade request up front (fail fast) --- */
        char key[24];
        WebSocketClientHandshake::generateKey(key);

        std::string protocolsCsv;
        for (size_t i = 0; i < behavior.protocols.size(); i++) {
            if (hasControlOrCRLF(behavior.protocols[i]) || behavior.protocols[i].find(',') != std::string::npos) {
                throw std::runtime_error("HEADER_INVALID");
            }
            if (i) {
                protocolsCsv += ", ";
            }
            protocolsCsv += behavior.protocols[i];
        }

        std::string request = WebSocketClientHandshake::buildUpgradeRequest(host, port, defaultPort,
            target, key, protocolsCsv, behavior.compression, behavior.headers);

        /* --- resolve the proxy (spec A.3b) ---
         * connect() consumes a fully-resolved per-connect ProxyConfig (host/port/authHeader). The addon's
         * proxy-URL parser fills behavior.proxy from the per-connect `proxy` option, or from this app's
         * `defaultProxy` (ClientOptions.proxy) when the connect omits its own. When a proxy is set, on_open
         * sends the CONNECT tunnel request first (built here, so a bad proxy would fail fast) and the WS
         * GET is deferred to the WS_UPGRADE phase. The WS request's Host: header is unchanged (destination),
         * so the destination sees exactly what it would on a direct dial. */
        bool useProxy = behavior.proxy.enabled;
        std::string proxyRequest;
        if (useProxy) {
            proxyRequest = WebSocketClientHandshake::buildConnectRequest(host, port, behavior.proxy.authHeader);
        }

        /* --- per-connect slot (bounds native growth; freed as a unit when the connection is terminal) --- */
        auto slotOwner = std::make_unique<ConnectSlot>();
        ConnectSlot *slot = slotOwner.get();
        ClientState<SSL> *statePtr = state.get();
        state->slots.push_back(std::move(slotOwner));

        /* --- frame context (per connect, child of parentContext) --- */
        auto *frameContext = WebSocketContext<SSL, false, UserData>::create(Loop::get(), state->parentContext, topicTree);
        /* WebSocketContext::free()/getSocketContext()/getExt() are private (App is a friend, we are not),
         * so we reach the context through its us_socket_context_t identity and inline the free. */
        us_socket_context_t *frameCtxRaw = (us_socket_context_t *) frameContext;
        slot->deleters.push_back([frameCtxRaw]() {
            ((WebSocketContextData<SSL, false, UserData> *) us_socket_context_ext(SSL, frameCtxRaw))->~WebSocketContextData();
            us_socket_context_free(SSL, frameCtxRaw);
        });
        slot->contexts.push_back((void *) frameCtxRaw);

        if (behavior.compression) {
            LoopData *loopData = (LoopData *) us_loop_ext(us_socket_context_loop(SSL, frameCtxRaw));
            if (!loopData->zlibContext) {
                loopData->zlibContext = new ZlibContext;
                loopData->inflationStream = new InflationStream(CompressOptions::DEDICATED_DECOMPRESSOR);
                loopData->deflationStream = new DeflationStream(CompressOptions::DEDICATED_COMPRESSOR);
            }
        }

        auto *frameExt = (WebSocketContextData<SSL, false, UserData> *) us_socket_context_ext(SSL, frameCtxRaw);
        frameExt->openHandler = std::move(behavior.open);
        frameExt->messageHandler = std::move(behavior.message);
        frameExt->droppedHandler = std::move(behavior.dropped);
        frameExt->drainHandler = std::move(behavior.drain);
        frameExt->subscriptionHandler = std::move(behavior.subscription);
        frameExt->closeHandler = std::move(behavior.close);
        /* Retire this slot once the frame socket's TRANSPORT close completes - not from closeHandler, which
         * end() emits early while the socket is still open (that would free the frame context under a live
         * socket). socketClosedHandler fires from WebSocketContext's on_close after the socket is torn down,
         * so the deferred sweep only ever frees an empty context. statePtr/slot stay valid: the frame
         * context is owned by this slot owned by statePtr, which outlives every socket it holds. */
        frameExt->socketClosedHandler = [slot, statePtr]() {
            statePtr->retire(slot);
        };
        frameExt->pingHandler = std::move(behavior.ping);
        frameExt->pongHandler = std::move(behavior.pong);
        frameExt->maxPayloadLength = behavior.maxPayloadLength;
        frameExt->maxBackpressure = behavior.maxBackpressure;
        frameExt->closeOnBackpressureLimit = behavior.closeOnBackpressureLimit;
        frameExt->resetIdleTimeoutOnSend = behavior.resetIdleTimeoutOnSend;
        frameExt->sendPingsAutomatically = behavior.sendPingsAutomatically;
        frameExt->maxLifetime = behavior.maxLifetime;
        frameExt->compression = behavior.compression;
        frameExt->calculateIdleTimeoutComponents(behavior.idleTimeout);

        /* --- dial options ---
         * When a proxy is set we dial the PROXY host/port (DNS, address fallback and connectTimeout all
         * apply to the proxy); the SNI/verification hostname is ALWAYS the destination, never the proxy
         * (spec A.3a us_connect_options_t). uSockets knows nothing about proxies - the two-phase behaviour
         * lives entirely in WebSocketClientContext. */
        std::string sni = behavior.servername.length() ? behavior.servername : host;

        /* Per-socket verify override for the SSL ext / start_tls: -1 (inherit) -> 0, 1 (verify) -> 1,
         * 0 (no verify) -> 2. Applied by ssl_on_open for a direct wss:// dial and by us_socket_start_tls
         * for a proxied wss:// dial. */
        int clientVerify = behavior.rejectUnauthorized < 0 ? 0 : (behavior.rejectUnauthorized ? 1 : 2);

        us_connect_options_t options = {};
        options.host = useProxy ? behavior.proxy.host.c_str() : host.c_str();
        options.port = useProxy ? behavior.proxy.port : port;
        options.source_host = behavior.sourceHost.length() ? behavior.sourceHost.c_str() : nullptr;
        options.sni_hostname = sni.c_str();
        options.timeout_ms = behavior.connectTimeout;
        options.handshake_timeout_ms = behavior.handshakeTimeout;
        options.client_verify = clientVerify;

        if constexpr (SSL) {
            if (useProxy) {
                /* Proxied wss://: the socket must open PLAINTEXT to run the CONNECT tunnel, then be adopted
                 * into an SSL handshake context where TLS is started (WebSocketClientContext::proxyStartTls).
                 * So we build BOTH per-connect handshake contexts: the SSL one (adoption target; owns the
                 * SSL frame adoption) and a plaintext CONNECT one (the dialing context; its sslPeer points
                 * at the SSL one). The failed callback lives on the plaintext context until CONNECT 200,
                 * when proxyStartTls hands it to the SSL context. */
                auto *sslHs = WebSocketClientContext<true, UserData>::create(state->parentContext);
                auto *plainHs = WebSocketClientContext<false, UserData>::create(state->parentContext);

                /* Deleter order: free the plaintext CONNECT context BEFORE its SSL peer so no socket is
                 * ever mid-adoption at teardown (spec A.3b ClientApp destructor). */
                slot->deleters.push_back([plainHs]() { plainHs->free(); });
                slot->deleters.push_back([sslHs]() { sslHs->free(); });
                slot->contexts.push_back((void *) plainHs);
                slot->contexts.push_back((void *) sslHs);

                sslHs->getExt()->frameContext = frameContext;
                /* A pre-open failure can surface on EITHER handshake context (the plaintext CONNECT phase or
                 * the SSL phase after proxyStartTls), so both retire the same slot; retire() is idempotent. */
                sslHs->getExt()->onTerminal = [slot, statePtr]() { statePtr->retire(slot); };

                auto *plainExt = plainHs->getExt();
                plainExt->sslPeer = (us_socket_context_t *) sslHs;
                plainExt->onTerminal = [slot, statePtr]() { statePtr->retire(slot); };
                plainExt->failedHandler = std::move(behavior.failed);
                plainExt->pending.request = std::move(request);
                plainExt->pending.proxyRequest = std::move(proxyRequest);
                plainExt->pending.destSni = sni;
                memcpy(plainExt->pending.key, key, sizeof(key));
                plainExt->pending.protocols = std::move(protocolsCsv);
                plainExt->pending.wanted = behavior.compression;
                plainExt->pending.user = behavior.userData;
                plainExt->pending.wantsTls = true;
                plainExt->pending.clientVerify = clientVerify;

                us_connect_request_t *r = us_socket_context_connect(false, (us_socket_context_t *) plainHs,
                    &options, (int) sizeof(HandshakeData<false>));
                if (!r) {
                    /* Defer failed to avoid re-entrancy inside connect(); always free userData because
                     * the context extension does not own it if the app is gone before this runs. */
                    void *user = plainExt->pending.user;
                    plainExt->pending.user = nullptr;
                    std::weak_ptr<ClientState<SSL>> weak = state;
                    auto *ctx = plainHs;
                    Loop::get()->defer([weak, ctx, slot, user]() {
                        if (auto self = weak.lock()) {
                            auto *ext = ctx->getExt();
                            if (ext->failedHandler) {
                                ClientConnectError err;
                                err.code = "URL_INVALID";
                                err.message = "connect could not be started";
                                ext->failedHandler(err);
                            }
                            self->retire(slot);
                        }
                        if (user) {
                            delete (UserData *) user;
                        }
                    });
                } else {
                    plainExt->pending.request_ = r;
                }

                slot->cancel = [plainHs]() {
                    auto *ext = plainHs->getExt();
                    us_connect_request_cancel(false, ext->pending.request_);
                    ext->pending.request_ = nullptr;
                };

                return std::move(static_cast<TemplatedClientApp &&>(*this));
            }
        }

        /* --- single-context path (per connect, child of parentContext) ---
         * Direct ws:// / wss://, or ws:// through a proxy. A proxied ws:// tunnel is a plain byte pipe (no
         * TLS), so the CONNECT and WS_UPGRADE phases share one plaintext context and need no adoption;
         * wantsTls stays false. A direct connect gets its TLS (if any) from ssl_on_open, not the proxy
         * phase, so this path is byte-for-byte the step-7 direct dial when no proxy is set. */
        auto *handshakeContext = WebSocketClientContext<SSL, UserData>::create(state->parentContext);
        slot->deleters.push_back([handshakeContext]() { handshakeContext->free(); });
        slot->contexts.push_back((void *) handshakeContext);

        auto *hsExt = handshakeContext->getExt();
        hsExt->frameContext = frameContext;
        hsExt->onTerminal = [slot, statePtr]() { statePtr->retire(slot); };
        hsExt->failedHandler = std::move(behavior.failed);
        hsExt->pending.request = std::move(request);
        hsExt->pending.proxyRequest = std::move(proxyRequest);
        hsExt->pending.destSni = sni;
        memcpy(hsExt->pending.key, key, sizeof(key));
        hsExt->pending.protocols = std::move(protocolsCsv);
        hsExt->pending.wanted = behavior.compression;
        hsExt->pending.user = behavior.userData;

        us_connect_request_t *r = us_socket_context_connect(SSL, (us_socket_context_t *) handshakeContext,
            &options, (int) sizeof(HandshakeData<SSL>));

        if (!r) {
            /* Defer failed to avoid re-entrancy inside connect(); always free userData because
             * the context extension does not own it if the app is gone before this runs. */
            void *user = hsExt->pending.user;
            hsExt->pending.user = nullptr;
            std::weak_ptr<ClientState<SSL>> weak = state;
            auto *ctx = handshakeContext;
            Loop::get()->defer([weak, ctx, slot, user]() {
                if (auto self = weak.lock()) {
                    auto *ext = ctx->getExt();
                    if (ext->failedHandler) {
                        ClientConnectError err;
                        err.code = "URL_INVALID";
                        err.message = "connect could not be started";
                        ext->failedHandler(err);
                    }
                    self->retire(slot);
                }
                if (user) {
                    delete (UserData *) user;
                }
            });
        } else {
            hsExt->pending.request_ = r;
        }

        /* close() cancels the request if it is still pending (no-op once opened/failed nulls it). */
        slot->cancel = [handshakeContext]() {
            auto *ext = handshakeContext->getExt();
            us_connect_request_cancel(SSL, ext->pending.request_);
            ext->pending.request_ = nullptr;
        };

        return std::move(static_cast<TemplatedClientApp &&>(*this));
    }
};

typedef TemplatedClientApp<false> ClientApp;
typedef TemplatedClientApp<true> SSLClientApp;

}

#endif // UWS_CLIENTAPP_H
