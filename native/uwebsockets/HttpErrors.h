#ifndef UWS_HTTP_ERRORS
#define UWS_HTTP_ERRORS

#include <cstddef>
#include <string_view>

namespace uWS {
/* Possible errors from http parsing */
enum HttpError {
    HTTP_ERROR_505_HTTP_VERSION_NOT_SUPPORTED = 1,
    HTTP_ERROR_431_REQUEST_HEADER_FIELDS_TOO_LARGE = 2,
    HTTP_ERROR_400_BAD_REQUEST = 3
};

/* Default cap on request line + headers, matching Node's --max-http-header-size. */
static constexpr size_t DEFAULT_MAX_HEADER_SIZE = 16 * 1024;
/* Hard ceiling for any configured value; mirrors the old env-var clamp. */
static constexpr size_t MAX_ALLOWED_HEADER_SIZE = 1024 * 1024;

#ifndef UWS_HTTPRESPONSE_NO_WRITEMARK

/* Returned parser errors match this LUT. */
static constexpr std::string_view httpErrorResponses[] = {
    "", /* Zeroth place is no error so don't use it */
    "HTTP/1.1 505 HTTP Version Not Supported\r\nConnection: close\r\n\r\n<h1>HTTP Version Not Supported</h1><p>This server does not support HTTP/1.0.</p><hr><i>uws Server</i>",
    "HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\n\r\n<h1>Request Header Fields Too Large</h1><hr><i>uws Server</i>",
    "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n<h1>Bad Request</h1><hr><i>uws Server</i>",
};

#else
/* Anonymized pages */
static constexpr std::string_view httpErrorResponses[] = {
    "", /* Zeroth place is no error so don't use it */
    "HTTP/1.1 505 HTTP Version Not Supported\r\nConnection: close\r\n\r\n",
    "HTTP/1.1 431 Request Header Fields Too Large\r\nConnection: close\r\n\r\n",
    "HTTP/1.1 400 Bad Request\r\nConnection: close\r\n\r\n"
};
#endif

}

#endif