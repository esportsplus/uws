#ifndef UWS_WEBSOCKETCONTEXTDATA_H
#define UWS_WEBSOCKETCONTEXTDATA_H

#include "Loop.h"
#include "AsyncSocket.h"

#include "MoveOnlyFunction.h"
#include <string_view>
#include <vector>

#include "WebSocketProtocol.h"
#include "TopicTree.h"
#include "WebSocketData.h"

namespace uWS {

/* Type queued up when publishing */
struct TopicTreeMessage {
    std::string message;
    /*OpCode*/ int opCode;
    bool compress;
};
struct TopicTreeBigMessage {
    std::string_view message;
    /*OpCode*/ int opCode;
    bool compress;
};

template <bool, bool, typename> struct WebSocket;

/* todo: this looks identical to WebSocketBehavior, why not just std::move that entire thing in? */

template <bool SSL, bool isServer, typename USERDATA>
struct WebSocketContextData {
private:

public:

    /* This one points to the App's shared topicTree */
    TopicTree<TopicTreeMessage, TopicTreeBigMessage> *topicTree;

    /* The callbacks for this context */
    MoveOnlyFunction<void(WebSocket<SSL, isServer, USERDATA> *)> openHandler = nullptr;
    MoveOnlyFunction<void(WebSocket<SSL, isServer, USERDATA> *, std::string_view, OpCode)> messageHandler = nullptr;
    MoveOnlyFunction<void(WebSocket<SSL, isServer, USERDATA> *, std::string_view, OpCode)> droppedHandler = nullptr;
    MoveOnlyFunction<void(WebSocket<SSL, isServer, USERDATA> *)> drainHandler = nullptr;
    MoveOnlyFunction<void(WebSocket<SSL, isServer, USERDATA> *, std::string_view, int, int)> subscriptionHandler = nullptr;
    MoveOnlyFunction<void(WebSocket<SSL, isServer, USERDATA> *, int, std::string_view)> closeHandler = nullptr;
    MoveOnlyFunction<void(WebSocket<SSL, isServer, USERDATA> *, std::string_view)> pingHandler = nullptr;
    MoveOnlyFunction<void(WebSocket<SSL, isServer, USERDATA> *, std::string_view)> pongHandler = nullptr;

    /* Fired once per socket AFTER its transport close fully completes (WebSocketData destroyed), for BOTH
     * the abrupt path and the graceful end() path - unlike closeHandler, which end() emits early while the
     * socket is still open. Null on server contexts; the outbound client sets it to retire the per-connect
     * slot only once the frame socket is truly gone, so a slot's contexts are never freed under a live
     * socket (spec production-fixes F1). */
    MoveOnlyFunction<void()> socketClosedHandler = nullptr;

    /* Settings for this context */
    size_t maxPayloadLength = 0;

    /* We do need these for async upgrade */
    CompressOptions compression;

    /* There needs to be a maxBackpressure which will force close everything over that limit */
    size_t maxBackpressure = 0;
    bool closeOnBackpressureLimit;
    bool resetIdleTimeoutOnSend;
    bool sendPingsAutomatically;
    unsigned short maxLifetime;

    /* These are calculated on creation */
    std::pair<unsigned short, unsigned short> idleTimeoutComponents;

    /* This is run once on start-up */
    void calculateIdleTimeoutComponents(unsigned short idleTimeout) {
        unsigned short margin = 4;

        if (!idleTimeout) {
            idleTimeoutComponents = {0, margin};
            return;
        }

        /* 4, 8 or 16 seconds margin based on idleTimeout */
        while ((int) idleTimeout - margin * 2 >= margin * 2 && margin < 16) {
            margin = (unsigned short) (margin << 1);
        }
        idleTimeoutComponents = {
            idleTimeout - (sendPingsAutomatically ? margin : 0), /* reduce normal idleTimeout if it is extended by ping-timeout */
            margin /* ping-timeout - also used for end() timeout */
        };
    }

    ~WebSocketContextData() {

    }

    WebSocketContextData(TopicTree<TopicTreeMessage, TopicTreeBigMessage> *topicTree) : topicTree(topicTree) {

    }
};

}

#endif // UWS_WEBSOCKETCONTEXTDATA_H
