/* Implements the common parser (RFC 822) used in both HTTP and Multipart parsing */

#ifndef UWS_MESSAGE_PARSER_H
#define UWS_MESSAGE_PARSER_H

#include <string_view>
#include <utility>
#include <cstring>

/* For now we have this one here */
#define MAX_HEADERS 100

namespace uWS {

    // should be templated on whether it needs at lest one header (http), or not (multipart)
    static inline unsigned int getHeaders(char *postPaddedBuffer, char *end, std::pair<std::string_view, std::string_view> *headers) {
        char *preliminaryKey, *preliminaryValue, *start = postPaddedBuffer;

        for (unsigned int i = 0; i < MAX_HEADERS; i++) {
            for (preliminaryKey = postPaddedBuffer; (*postPaddedBuffer != ':') & (*(unsigned char *)postPaddedBuffer > 32); *(postPaddedBuffer++) |= 32);
            if (*postPaddedBuffer == '\r') {
                if ((postPaddedBuffer != end) & (postPaddedBuffer[1] == '\n') /* & (i > 0) */) { // multipart does not require any headers like http does
                    headers->first = std::string_view(nullptr, 0);
                    return (unsigned int) ((postPaddedBuffer + 2) - start);
                } else {
                    return 0;
                }
            } else {
                headers->first = std::string_view(preliminaryKey, (size_t) (postPaddedBuffer - preliminaryKey));
                for (postPaddedBuffer++; (*postPaddedBuffer == ':' || *(unsigned char *)postPaddedBuffer < 33) && *postPaddedBuffer != '\r'; postPaddedBuffer++);
                preliminaryValue = postPaddedBuffer;
                postPaddedBuffer = (char *) memchr(postPaddedBuffer, '\r', end - postPaddedBuffer);
                if (postPaddedBuffer && postPaddedBuffer[1] == '\n') {
                    headers->second = std::string_view(preliminaryValue, (size_t) (postPaddedBuffer - preliminaryValue));
                    postPaddedBuffer += 2;
                    headers++;
                } else {
                    return 0;
                }
            }
        }
        return 0;
    }

}

#endif
