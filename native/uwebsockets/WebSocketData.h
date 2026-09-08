#ifndef UWS_WEBSOCKETDATA_H
#define UWS_WEBSOCKETDATA_H

#include "WebSocketProtocol.h"
#include "AsyncSocketData.h"
#include "PerMessageDeflate.h"
#include "TopicTree.h"

#include <cstring>
#include <string>

namespace uWS {

template <bool isServer>
struct WebSocketData : AsyncSocketData<false>, WebSocketState<isServer> {
    /* This guy has a lot of friends - why? */
    template <bool, bool, typename> friend struct WebSocketContext;
    template <bool, bool, typename> friend struct WebSocketContextData;
    template <bool, bool, typename> friend struct WebSocket;
    template <bool> friend struct HttpContext;
private:
    std::string fragmentBuffer;
    unsigned int controlTipLength = 0;
    bool isShuttingDown = 0;
    bool hasTimedOut = false;
    enum CompressionStatus : char {
        DISABLED,
        ENABLED,
        COMPRESSED_FRAME
    } compressionStatus;

    /* We might have a dedicated compressor */
    DeflationStream *deflationStream = nullptr;
    /* And / or a dedicated decompressor */
    InflationStream *inflationStream = nullptr;

    /* We could be a subscriber */
    Subscriber *subscriber = nullptr;
public:
    template <bool SSL>
    WebSocketData(bool perMessageDeflate, CompressOptions compressOptions, AsyncSocketData<SSL> &&asyncSocketData) : AsyncSocketData<false>(std::move(asyncSocketData.buffer)), WebSocketState<isServer>() {
#ifdef UWS_REMOTE_ADDRESS_USERSPACE
        memcpy(remoteAddress, asyncSocketData.remoteAddress, sizeof(remoteAddress));
        remoteAddressLength = asyncSocketData.remoteAddressLength;
#endif

        compressionStatus = perMessageDeflate ? ENABLED : DISABLED;

        /* Initialize the dedicated sliding window(s) */
        if (perMessageDeflate) {
            if ((compressOptions & CompressOptions::_COMPRESSOR_MASK) != CompressOptions::SHARED_COMPRESSOR) {
                deflationStream = new DeflationStream(compressOptions);
            }
            if ((compressOptions & CompressOptions::_DECOMPRESSOR_MASK) != CompressOptions::SHARED_DECOMPRESSOR) {
                inflationStream = new InflationStream(compressOptions);
            }
        }
    }

    ~WebSocketData() {
        if (deflationStream) {
            delete deflationStream;
        }

        if (inflationStream) {
            delete inflationStream;
        }

        if (subscriber) {
            delete subscriber;
        }
    }
};

}

#endif // UWS_WEBSOCKETDATA_H
