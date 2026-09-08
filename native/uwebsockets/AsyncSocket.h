#ifndef UWS_ASYNCSOCKET_H
#define UWS_ASYNCSOCKET_H

/* This class implements async socket memory management strategies */

/* NOTE: Many unsigned/signed conversion warnings could be solved by moving from int length
 * to unsigned length for everything to/from uSockets - this would however remove the opportunity
 * to signal error with -1 (which is how the entire UNIX syscalling is built). */

#include <cstring>
#include <iostream>

#include "libusockets.h"

#include "LoopData.h"
#include "AsyncSocketData.h"

namespace uWS {

    enum SendBufferAttribute {
        NEEDS_NOTHING,
        NEEDS_DRAIN,
        NEEDS_UNCORK
    };

    template <bool, bool, typename> struct WebSocketContext;

template <bool SSL>
struct AsyncSocket {
    /* This guy is promiscuous */
    template <bool> friend struct HttpContext;
    template <bool, bool, typename> friend struct WebSocketContext;
    template <bool> friend struct TemplatedApp;
    template <bool> friend struct TemplatedClientApp;
    template <bool, bool, typename> friend struct WebSocketContextData;
    template <typename, typename> friend struct TopicTree;
    template <bool> friend struct HttpResponse;

private:
    /* Helper, do not use directly (todo: move to uSockets or de-crazify) */
    void throttle_helper(int toggle) {
        struct us_poll_t *p = (struct us_poll_t *) this;
        struct us_loop_t *loop = us_socket_context_loop(SSL, us_socket_context(SSL, (us_socket_t *) this));
        AsyncSocketData<SSL> *asyncSocketData = getAsyncSocketData();

        if (toggle) {
            /* Pause */
            int events = us_poll_events(p);
            if (events) {
                asyncSocketData->savedEvents = events;
            }
            us_poll_change(p, loop, 0);
        } else {
            /* Resume */
            int events = asyncSocketData->savedEvents;
            us_poll_change(p, loop, events);
        }
    }

protected:
    /* Returns SSL pointer or FD as pointer */
    void *getNativeHandle() {
        return us_socket_get_native_handle(SSL, (us_socket_t *) this);
    }

    /* Get loop data for socket */
    LoopData *getLoopData() {
        return (LoopData *) us_loop_ext(us_socket_context_loop(SSL, us_socket_context(SSL, (us_socket_t *) this)));
    }

    /* Get socket extension */
    AsyncSocketData<SSL> *getAsyncSocketData() {
        return (AsyncSocketData<SSL> *) us_socket_ext(SSL, (us_socket_t *) this);
    }

    /* Socket timeout */
    void timeout(unsigned int seconds) {
        us_socket_timeout(SSL, (us_socket_t *) this, seconds);
    }

    /* Shutdown socket without any automatic drainage */
    void shutdown() {
        us_socket_shutdown(SSL, (us_socket_t *) this);
    }

    /* Experimental pause */
    us_socket_t *pause() {
        throttle_helper(1);
        return (us_socket_t *) this;
    }

    /* Experimental resume */
    us_socket_t *resume() {
        throttle_helper(0);
        return (us_socket_t *) this;
    }

    /* Immediately close socket */
    us_socket_t *close() {
        return us_socket_close(SSL, (us_socket_t *) this, 0, nullptr);
    }

    void corkUnchecked() {
        /* What if another socket is corked? */
        getLoopData()->corkedSocket = this;
    }

    void uncorkWithoutSending() {
        if (isCorked()) {
            getLoopData()->corkedSocket = nullptr;
        }
    }

    /* Cork this socket. Only one socket may ever be corked per-loop at any given time */
    void cork() {
        /* Extra check for invalid corking of others */
        if (getLoopData()->corkOffset && getLoopData()->corkedSocket != this) {
            std::cerr << "Error: Cork buffer must not be acquired without checking canCork!" << std::endl;
            std::terminate();
        }

        /* What if another socket is corked? */
        getLoopData()->corkedSocket = this;
    }

    /* Returns whether we are corked or not */
    bool isCorked() {
        return getLoopData()->corkedSocket == this;
    }

    /* Returns whether we could cork (it is free) */
    bool canCork() {
        return getLoopData()->corkedSocket == nullptr;
    }

    /* Returns a suitable buffer for temporary assemblation of send data */
    std::pair<char *, SendBufferAttribute> getSendBuffer(size_t size) {
        /* First step is to determine if we already have backpressure or not */
        LoopData *loopData = getLoopData();
        BackPressure &backPressure = getAsyncSocketData()->buffer;
        size_t existingBackpressure = backPressure.length();
        if ((!existingBackpressure) && (isCorked() || canCork()) && (loopData->corkOffset + size < LoopData::CORK_BUFFER_SIZE)) {
            /* Cork automatically if we can */
            if (isCorked()) {
                char *sendBuffer = loopData->corkBuffer + loopData->corkOffset;
                loopData->corkOffset += (unsigned int) size;
                return {sendBuffer, SendBufferAttribute::NEEDS_NOTHING};
            } else {
                cork();
                char *sendBuffer = loopData->corkBuffer + loopData->corkOffset;
                loopData->corkOffset += (unsigned int) size;
                return {sendBuffer, SendBufferAttribute::NEEDS_UNCORK};
            }
        } else {

            /* If we are corked and there is already data in the cork buffer,
            mark how much is ours and reset it */
            unsigned int ourCorkOffset = 0;
            if (isCorked() && loopData->corkOffset) {
                ourCorkOffset = loopData->corkOffset;
                loopData->corkOffset = 0;
            }

            /* Fallback is to use the backpressure as buffer */
            backPressure.resize(ourCorkOffset + existingBackpressure + size);

            /* And copy corkbuffer in front */
            memcpy((char *) backPressure.data() + existingBackpressure, loopData->corkBuffer, ourCorkOffset);

            return {(char *) backPressure.data() + ourCorkOffset + existingBackpressure, SendBufferAttribute::NEEDS_DRAIN};
        }
    }

    /* Returns the user space backpressure. */
    unsigned int getBufferedAmount() {
        /* We return the actual amount of bytes in backbuffer, including pendingRemoval */
        return (unsigned int) getAsyncSocketData()->buffer.totalLength();
    }

    /* Returns the text representation of an IPv4 or IPv6 address */
    std::string_view addressAsText(std::string_view binary) {
        static thread_local char buf[64];
        int ipLength = 0;

        if (!binary.length()) {
            return {};
        }

        unsigned char *b = (unsigned char *) binary.data();

        if (binary.length() == 4) {
            ipLength = snprintf(buf, 64, "%u.%u.%u.%u", b[0], b[1], b[2], b[3]);
        } else {
            /* IPv4-mapped IPv6 addresses are represented as IPv4. */
            if (!memcmp(b, "\0\0\0\0\0\0\0\0\0\0\xff\xff", 12)) {
                ipLength = snprintf(buf, 64, "%u.%u.%u.%u", b[12], b[13], b[14], b[15]);
            } else {
                unsigned int groups[8];
                int longestStart = -1;
                int longestLength = 0;

                for (int i = 0; i < 8; i++) {
                    groups[i] = ((unsigned int) b[i * 2] << 8) | b[i * 2 + 1];
                }

                /* Find the longest zero run; keeping the first one resolves ties. */
                for (int i = 0; i < 8;) {
                    if (groups[i]) {
                        i++;
                        continue;
                    }

                    int start = i;
                    while (i < 8 && !groups[i]) {
                        i++;
                    }

                    if (i - start > longestLength) {
                        longestStart = start;
                        longestLength = i - start;
                    }
                }

                char *p = buf;
                for (int i = 0; i < 8; i++) {
                    if (longestLength >= 2 && i == longestStart) {
                        *p++ = ':';
                        *p++ = ':';
                        i += longestLength - 1;
                    } else {
                        if (p != buf && p[-1] != ':') {
                            *p++ = ':';
                        }
                        p += snprintf(p, (size_t) (buf + sizeof(buf) - p), "%x", groups[i]);
                    }
                }
                ipLength = (int) (p - buf);
            }
        }

        return {buf, (unsigned int) ipLength};
    }

    /* Returns the remote IP address or empty string on failure */
    std::string_view getRemoteAddress() {
#ifdef UWS_REMOTE_ADDRESS_USERSPACE
        AsyncSocketData<SSL> *data = getAsyncSocketData();
        return std::string_view(data->remoteAddress, (unsigned int) data->remoteAddressLength);
#else
        static thread_local char buf[16];
        int ipLength = 16;
        us_socket_remote_address(SSL, (us_socket_t *) this, buf, &ipLength);
        return std::string_view(buf, (unsigned int) ipLength);
#endif
    }

    /* Returns the text representation of IP */
    std::string_view getRemoteAddressAsText() {
        return addressAsText(getRemoteAddress());
    }

    /* Returns the remote port number or -1 on failure */
    unsigned int getRemotePort() {
        int port = us_socket_remote_port(SSL, (us_socket_t *) this);
        return (unsigned int) port;
    }

    /* Write in three levels of prioritization: cork-buffer, syscall, socket-buffer. Always drain if possible.
     * Returns pair of bytes written (anywhere) and whether or not this call resulted in the polling for
     * writable (or we are in a state that implies polling for writable). */
    std::pair<int, bool> write(const char *src, int length, bool optionally = false, int nextLength = 0) {
        /* Fake success if closed, simple fix to allow uncork of closed socket to succeed */
        if (us_socket_is_closed(SSL, (us_socket_t *) this)) {
            return {length, false};
        }

        LoopData *loopData = getLoopData();
        AsyncSocketData<SSL> *asyncSocketData = getAsyncSocketData();

        /* We are limited if we have a per-socket buffer */
        if (asyncSocketData->buffer.length()) {
            /* Write off as much as we can */
            int written = us_socket_write(SSL, (us_socket_t *) this, asyncSocketData->buffer.data(), (int) asyncSocketData->buffer.length(), /*nextLength != 0 | */length);

            /* On failure return, otherwise continue down the function */
            if ((unsigned int) written < asyncSocketData->buffer.length()) {

                /* Update buffering (todo: we can do better here if we keep track of what happens to this guy later on) */
                asyncSocketData->buffer.erase((unsigned int) written);

                if (optionally) {
                    /* Thankfully we can exit early here */
                    return {0, true};
                } else {
                    /* This path is horrible and points towards erroneous usage */
                    asyncSocketData->buffer.append(src, (unsigned int) length);

                    return {length, true};
                }
            }

            /* At this point we simply have no buffer and can continue as normal */
            asyncSocketData->buffer.clear();
        }

        if (length) {
            if (loopData->corkedSocket == this) {
                /* We are corked */
                if (LoopData::CORK_BUFFER_SIZE - loopData->corkOffset >= (unsigned int) length) {
                    /* If the entire chunk fits in cork buffer */
                    memcpy(loopData->corkBuffer + loopData->corkOffset, src, (unsigned int) length);
                    loopData->corkOffset += (unsigned int) length;
                    /* Fall through to default return */
                } else {
                    /* We take the penalty of two syscalls */
                    return uncork(src, length, optionally);
                }
            } else {
                /* We are not corked */
                int written = us_socket_write(SSL, (us_socket_t *) this, src, length, nextLength != 0);

                /* Did we fail? */
                if (written < length) {
                    /* If the write was optional then just bail out */
                    if (optionally) {
                        return {written, true};
                    }

                    /* Fall back to worst possible case (should be very rare for HTTP) */
                    /* At least we can reserve room for next chunk if we know it up front */
                    if (nextLength) {
                        asyncSocketData->buffer.reserve(asyncSocketData->buffer.length() + (size_t) (length - written + nextLength));
                    }

                    /* Buffer this chunk */
                    asyncSocketData->buffer.append(src + written, (size_t) (length - written));

                    /* Return the failure */
                    return {length, true};
                }
                /* Fall through to default return */
            }
        }

        /* Default fall through return */
        return {length, false};
    }

    /* Uncork this socket and flush or buffer any corked and/or passed data. It is essential to remember doing this. */
    /* It does NOT count bytes written from cork buffer (they are already accounted for in the write call responsible for its corking)! */
    std::pair<int, bool> uncork(const char *src = nullptr, int length = 0, bool optionally = false) {
        LoopData *loopData = getLoopData();

        if (loopData->corkedSocket == this) {
            loopData->corkedSocket = nullptr;

            if (loopData->corkOffset) {
                /* Corked data is already accounted for via its write call */
                auto [written, failed] = write(loopData->corkBuffer, (int) loopData->corkOffset, false, length);
                loopData->corkOffset = 0;

                if (failed) {
                    /* If corked data fails to flush, and we have more data to write, immediately buffer it here 
                     * since the above call to write excludes src */
                    if (!optionally && src && length) {
                        AsyncSocketData<SSL> *asyncSocketData = getAsyncSocketData();
                        asyncSocketData->buffer.append(src, (size_t) length);

                        /* We wrote to per socket buffer, so report success */
                        return {length, true};
                    }

                    /* We do not need to care for buffering (of the corked data) here, write did that */
                    return {0, true};
                }
            }

            /* We should only return with new writes, not things written to cork already */
            return write(src, length, optionally, 0);
        } else {
            /* We are not even corked! */
            return {0, false};
        }
    }
};

}

#endif // UWS_ASYNCSOCKET_H
