#ifndef UWS_CHUNKEDENCODING_H
#define UWS_CHUNKEDENCODING_H

/* Independent chunked encoding parser, used by HttpParser. */

#include <string>
#include <cstring>
#include <cstdlib>
#include <algorithm>
#include <string_view>
#include "MoveOnlyFunction.h"
#include <optional>

namespace uWS {

    constexpr uint64_t STATE_HAS_SIZE = 1ull << (sizeof(uint64_t) * 8 - 1);//0x80000000;
    constexpr uint64_t STATE_IS_CHUNKED = 1ull << (sizeof(uint64_t) * 8 - 2);//0x40000000;
    constexpr uint64_t STATE_IS_EXTENSION = 1ull << (sizeof(uint64_t) * 8 - 3);
    constexpr uint64_t STATE_IS_TRAILER = 1ull << (sizeof(uint64_t) * 8 - 4);
    constexpr uint64_t STATE_SIZE_MASK = ~(0x0full << (sizeof(uint64_t) * 8 - 4));//0x3FFFFFFF;
    constexpr uint64_t STATE_IS_ERROR = ~0ull;//0xFFFFFFFF;
    constexpr uint64_t STATE_SIZE_OVERFLOW = 0x0Full << (sizeof(uint64_t) * 8 - 8);//0x0F000000;

    inline uint64_t chunkSize(uint64_t state) {
        return state & STATE_SIZE_MASK;
    }

    /* Reads hex number until CR or out of data to consume. Updates state. Returns bytes consumed. */
    inline void consumeHexNumber(std::string_view &data, uint64_t &state) {
        /* Consume the hexadecimal number, unless we are already skipping an extension. */
        if (!(state & STATE_IS_EXTENSION)) {
            while (data.length()) {
                unsigned char digit = (unsigned char) data.data()[0];
                unsigned int number;

                if (digit >= '0' && digit <= '9') {
                    number = digit - '0';
                } else if (digit >= 'A' && digit <= 'F') {
                    number = digit - 'A' + 10;
                } else if (digit >= 'a' && digit <= 'f') {
                    number = digit - 'a' + 10;
                } else if (digit == ';') {
                    state |= STATE_IS_EXTENSION;
                    data.remove_prefix(1);
                    break;
                } else if (digit == '\r' || digit == '\n') {
                    break;
                } else {
                    state = STATE_IS_ERROR;
                    return;
                }

                if (chunkSize(state) & STATE_SIZE_OVERFLOW) {
                    state = STATE_IS_ERROR;
                    return;
                }

                // extract state bits
                uint64_t bits = /*state &*/ STATE_IS_CHUNKED;

                state = (state & STATE_SIZE_MASK) * 16ull + number;

                state |= bits;
                data.remove_prefix(1);
            }
        }
        /* Consume everything not /n */
        while (data.length() && data.data()[0] != '\n') {
            data.remove_prefix(1);
        }
        /* Now we stand on \n so consume it and enable size */
        if (data.length()) {
            state += 2; // include the two last /r/n
            state &= ~STATE_IS_EXTENSION;
            state |= STATE_HAS_SIZE | STATE_IS_CHUNKED;
            data.remove_prefix(1);
        }
    }

    inline void decChunkSize(uint64_t &state, unsigned int by) {

        //unsigned int bits = state & STATE_IS_CHUNKED;

        state = (state & ~STATE_SIZE_MASK) | (chunkSize(state) - by);

        //state |= bits;
    }

    inline bool hasChunkSize(uint64_t state) {
        return state & STATE_HAS_SIZE;
    }

    /* Are we in the middle of parsing chunked encoding? */
    inline bool isParsingChunkedEncoding(uint64_t state) {
        return state & ~STATE_SIZE_MASK;
    }

    inline bool isParsingInvalidChunkedEncoding(uint64_t state) {
        return state == STATE_IS_ERROR;
    }

    /* Returns next chunk (empty or not), or if all data was consumed, nullopt is returned. */
    static std::optional<std::string_view> getNextChunk(std::string_view &data, uint64_t &state) {
        while (data.length()) {

            // HttpParser consumes trailer headers, since they can span input segments.
            if (state & STATE_IS_TRAILER) {
                return std::nullopt;
            }

            if (!hasChunkSize(state)) {
                consumeHexNumber(data, state);
                if (isParsingInvalidChunkedEncoding(state)) {
                    return std::nullopt;
                }
                if (hasChunkSize(state) && chunkSize(state) == 2) {

                    state = STATE_IS_TRAILER;

                    return std::string_view(nullptr, 0);
                }
                continue;
            }

            // do we have data to emit all?
            if (data.length() >= chunkSize(state)) {
                /* A remaining size of 1 means CR was consumed in the previous input */
                if ((chunkSize(state) >= 2 && data[chunkSize(state) - 2] != '\r') || data[chunkSize(state) - 1] != '\n') {
                    state = STATE_IS_ERROR;
                    return std::nullopt;
                }

                // emit all but 2 bytes then reset state to 0 and goto beginning
                // not fin
                std::string_view emitSoon;
                bool shouldEmit = false;
                if (chunkSize(state) > 2) {
                    emitSoon = std::string_view(data.data(), chunkSize(state) - 2);
                    shouldEmit = true;
                }
                data.remove_prefix(chunkSize(state));
                state = STATE_IS_CHUNKED;
                if (shouldEmit) {
                    return emitSoon;
                }
                continue;
            } else {
                /* Validate CR even when LF arrives in the next input */
                if (data.length() == chunkSize(state) - 1 && data.back() != '\r') {
                    state = STATE_IS_ERROR;
                    return std::nullopt;
                }

                /* We will consume all our input data */
                std::string_view emitSoon;
                if (chunkSize(state) > 2) {
                    uint64_t maximalAppEmit = chunkSize(state) - 2;
                    if (data.length() > maximalAppEmit) {
                        emitSoon = data.substr(0, maximalAppEmit);
                    } else {
                        //cb(data);
                        emitSoon = data;
                    }
                }
                decChunkSize(state, (unsigned int) data.length());
                state |= STATE_IS_CHUNKED;
                // new: decrease data by its size (bug)
                data.remove_prefix(data.length()); // ny bug fix för getNextChunk
                if (emitSoon.length()) {
                    return emitSoon;
                } else {
                    return std::nullopt;
                }
            }
        }

        return std::nullopt;
    }

    /* This is really just a wrapper for convenience */
    struct ChunkIterator {

        std::string_view *data;
        std::optional<std::string_view> chunk;
        uint64_t *state;

        ChunkIterator(std::string_view *data, uint64_t *state) : data(data), state(state) {
            chunk = uWS::getNextChunk(*data, *state);
        }

        ChunkIterator() {

        }

        ChunkIterator begin() {
            return *this;
        }

        ChunkIterator end() {
            return ChunkIterator();
        }

        std::string_view operator*() {
            if (!chunk.has_value()) {
                std::abort();
            }
            return chunk.value();
        }

        bool operator!=(const ChunkIterator &other) const {
            return other.chunk.has_value() != chunk.has_value();
        }

        ChunkIterator &operator++() {
            chunk = uWS::getNextChunk(*data, *state);
            return *this;
        }

    };
}

#endif // UWS_CHUNKEDENCODING_H
