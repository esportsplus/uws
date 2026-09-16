#ifndef UWS_HTTPCONTEXTDATA_H
#define UWS_HTTPCONTEXTDATA_H

#include "HttpErrors.h"
#include "HttpRouter.h"

#include <vector>
#include "MoveOnlyFunction.h"

namespace uWS {
template<bool> struct HttpResponse;
struct HttpRequest;

template <bool SSL>
struct alignas(16) HttpContextData {
    template <bool> friend struct HttpContext;
    template <bool> friend struct HttpResponse;
    template <bool> friend struct TemplatedApp;
private:
    std::vector<MoveOnlyFunction<void(HttpResponse<SSL> *, int)>> filterHandlers;

    MoveOnlyFunction<void(const char *hostname)> missingServerNameHandler;

    struct RouterData {
        HttpResponse<SSL> *httpResponse;
        HttpRequest *httpRequest;
    };

    /* This is the currently browsed-to router when using SNI */
    HttpRouter<RouterData> *currentRouter = &router;

    /* This is the default router for default SNI or non-SSL */
    HttpRouter<RouterData> router;
    void *upgradedWebSocket = nullptr;
    bool isParsingHttp = false;
    /* Per-app request header byte cap; set once by TemplatedApp at construction. */
    size_t maxHeaderSize = DEFAULT_MAX_HEADER_SIZE;
};

}

#endif // UWS_HTTPCONTEXTDATA_H
