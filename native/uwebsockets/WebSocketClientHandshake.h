#ifndef UWS_WEBSOCKETCLIENTHANDSHAKE_H
#define UWS_WEBSOCKETCLIENTHANDSHAKE_H

/* Client-side RFC 6455 handshake helpers (spec A.3b): build the outgoing GET upgrade request and parse
 * plus validate the incoming 101 response. Deliberately standalone - the response parser is NOT
 * HttpParser::getHeaders (which is request-shaped and mutates the buffer in place). This header only
 * understands a single HTTP/1.1 status line followed by headers; it is not a general HTTP client. */

#include <cstdint>
#include <cstring>
#include <cctype>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <openssl/rand.h>

#include "WebSocketHandshake.h"
#include "WebSocketExtensions.h"
#include "PerMessageDeflate.h"

namespace uWS {

struct WebSocketClientHandshake {

    /* --- small ASCII helpers (locale-independent) --- */

    static bool iequals(std::string_view a, std::string_view b) {
        if (a.length() != b.length()) {
            return false;
        }
        for (size_t i = 0; i < a.length(); i++) {
            if (tolower((unsigned char) a[i]) != tolower((unsigned char) b[i])) {
                return false;
            }
        }
        return true;
    }

    static std::string_view trim(std::string_view v) {
        while (v.length() && (v.front() == ' ' || v.front() == '\t')) {
            v.remove_prefix(1);
        }
        while (v.length() && (v.back() == ' ' || v.back() == '\t' || v.back() == '\r')) {
            v.remove_suffix(1);
        }
        return v;
    }

    /* Case-insensitive search for a comma/space separated token inside a header value. Used for the
     * "Upgrade: websocket" and "Connection: Upgrade" checks which may carry several tokens. */
    static bool hasToken(std::string_view value, std::string_view token) {
        size_t i = 0;
        while (i < value.length()) {
            /* Skip separators */
            while (i < value.length() && (value[i] == ',' || value[i] == ' ' || value[i] == '\t')) {
                i++;
            }
            size_t start = i;
            while (i < value.length() && value[i] != ',' && value[i] != ' ' && value[i] != '\t') {
                i++;
            }
            if (i > start && iequals(value.substr(start, i - start), token)) {
                return true;
            }
        }
        return false;
    }

    /* --- reserved request headers (users may not override these) --- */

    static bool isReservedHeader(std::string_view name) {
        static const char *reserved[] = {
            "host", "upgrade", "connection",
            "sec-websocket-key", "sec-websocket-version",
            "sec-websocket-protocol", "sec-websocket-extensions"
        };
        for (const char *r : reserved) {
            if (iequals(name, r)) {
                return true;
            }
        }
        return false;
    }

    /* A user header is rejected if the name/value carry CR/LF (header injection) or the name is reserved. */
    static bool isValidUserHeader(std::string_view name, std::string_view value) {
        if (!name.length() || isReservedHeader(name)) {
            return false;
        }
        for (char c : name) {
            if (c == '\r' || c == '\n' || c == ':') {
                return false;
            }
        }
        for (char c : value) {
            if (c == '\r' || c == '\n') {
                return false;
            }
        }
        return true;
    }

    /* --- Sec-WebSocket-Key: base64 of 16 strong random bytes --- */

    static void generateKey(char key[24]) {
        unsigned char bytes[16];
        RAND_bytes(bytes, sizeof(bytes));

        static const char *b64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        char *dst = key;
        for (int i = 0; i < 15; i += 3) {
            *dst++ = b64[(bytes[i] >> 2) & 63];
            *dst++ = b64[((bytes[i] & 3) << 4) | ((bytes[i + 1] & 240) >> 4)];
            *dst++ = b64[((bytes[i + 1] & 15) << 2) | ((bytes[i + 2] & 192) >> 6)];
            *dst++ = b64[bytes[i + 2] & 63];
        }
        /* 16th byte (index 15) fills the final quad with one pad char */
        *dst++ = b64[(bytes[15] >> 2) & 63];
        *dst++ = b64[(bytes[15] & 3) << 4];
        *dst++ = '=';
        *dst++ = '=';
    }

    /* --- request builder (spec A.3b "Request builder") ---
     * host/port/target and the offered protocols/headers are assumed already validated by the caller
     * (ClientApp::connect rejects reserved names and CR/LF). defaultPort omits the ":port" from Host. */
    static std::string buildUpgradeRequest(std::string_view host, int port, bool defaultPort,
            std::string_view target, const char key[24], std::string_view protocolsCsv,
            CompressOptions wanted, const std::vector<std::pair<std::string, std::string>> &userHeaders) {

        std::string req;
        req.reserve(256);

        req += "GET ";
        req += (target.length() ? target : std::string_view("/"));
        req += " HTTP/1.1\r\n";

        req += "Host: ";
        bool ipv6 = (host.find(':') != std::string_view::npos) && host.front() != '[';
        if (ipv6) {
            req += '[';
        }
        req += host;
        if (ipv6) {
            req += ']';
        }
        if (!defaultPort) {
            req += ":";
            req += std::to_string(port);
        }
        req += "\r\n";

        req += "Upgrade: websocket\r\n";
        req += "Connection: Upgrade\r\n";

        req += "Sec-WebSocket-Key: ";
        req.append(key, 24);
        req += "\r\n";

        req += "Sec-WebSocket-Version: 13\r\n";

        if (protocolsCsv.length()) {
            req += "Sec-WebSocket-Protocol: ";
            req += protocolsCsv;
            req += "\r\n";
        }

        /* permessage-deflate offer, derived from behavior.compression with the same bit unpacking as
         * HttpResponse.h:254-261. server_* describe the peer's compressor (our inflation window),
         * client_* describe our compressor (our deflation window). */
        if (wanted != DISABLED) {
            int wantedInflationWindow = 0;
            if ((wanted & CompressOptions::_DECOMPRESSOR_MASK) != CompressOptions::SHARED_DECOMPRESSOR) {
                wantedInflationWindow = (wanted & CompressOptions::_DECOMPRESSOR_MASK) >> 8;
            }
            int wantedCompressionWindow = (wanted & CompressOptions::_COMPRESSOR_MASK) >> 4;

            req += "Sec-WebSocket-Extensions: permessage-deflate";
            if (wantedInflationWindow == 0) {
                /* Shared decompressor: ask the server to reset its compressor each message so we can
                 * inflate without keeping a per-connection sliding window. */
                req += "; server_no_context_takeover";
            } else if (wantedInflationWindow < 15) {
                req += "; server_max_window_bits=";
                req += std::to_string(wantedInflationWindow);
            }
            if (wantedCompressionWindow == 0) {
                req += "; client_no_context_takeover";
            } else if (wantedCompressionWindow < 15) {
                req += "; client_max_window_bits=";
                req += std::to_string(wantedCompressionWindow);
            } else {
                /* Full window but still advertise that we understand the parameter. */
                req += "; client_max_window_bits";
            }
            req += "\r\n";
        }

        for (auto &h : userHeaders) {
            req += h.first;
            req += ": ";
            req += h.second;
            req += "\r\n";
        }

        req += "\r\n";
        return req;
    }

    /* --- proxy CONNECT request builder (spec A.3b "Proxy request builder") ---
     * Emits "CONNECT desthost:destport HTTP/1.1\r\nHost: desthost:destport\r\n" (port always explicit
     * per RFC 7231 §4.3.6; IPv6 literals bracketed) plus an optional Proxy-Authorization line. The auth
     * header VALUE (e.g. "Basic <base64(user:pass)>") is built by the addon in step 12 and passed in
     * ready here as authHeaderOrEmpty; empty means no userinfo on the proxy URL. */
    static std::string buildConnectRequest(std::string_view desthost, int destport,
            std::string_view authHeaderOrEmpty) {

        std::string req;
        req.reserve(128);

        /* An unbracketed ':' in the host means an IPv6 literal that must be bracketed in the target. */
        bool ipv6 = (desthost.find(':') != std::string_view::npos) && desthost.front() != '[';

        auto emitAuthority = [&]() {
            if (ipv6) {
                req += '[';
                req += desthost;
                req += ']';
            } else {
                req += desthost;
            }
            req += ':';
            req += std::to_string(destport);
        };

        req += "CONNECT ";
        emitAuthority();
        req += " HTTP/1.1\r\n";

        req += "Host: ";
        emitAuthority();
        req += "\r\n";

        if (authHeaderOrEmpty.length()) {
            req += "Proxy-Authorization: ";
            req += authHeaderOrEmpty;
            req += "\r\n";
        }

        req += "\r\n";
        return req;
    }

    /* --- response parser (spec A.3b, ~status line + headers) --- */

    struct ResponseHead {
        bool complete = false;                                    /* did we see the "\r\n\r\n" terminator */
        int status = 0;
        std::string_view reason;
        std::vector<std::pair<std::string, std::string>> headers; /* keys lower-cased, values trimmed */
        size_t headerBytes = 0;                                   /* offset just past the "\r\n\r\n" */
    };

    static std::string_view findHeader(const ResponseHead &head, std::string_view name) {
        for (auto &h : head.headers) {
            if (iequals(h.first, name)) {
                return h.second;
            }
        }
        return {};
    }

    /* Returns false only on a malformed status line; sets head.complete=false when the terminator has
     * not arrived yet (caller keeps buffering up to the 8 KiB cap). */
    static bool parseResponse(const char *data, size_t length, ResponseHead &head) {
        /* Locate the end of headers */
        const char *end = nullptr;
        if (length >= 4) {
            for (size_t i = 0; i + 4 <= length; i++) {
                if (data[i] == '\r' && data[i + 1] == '\n' && data[i + 2] == '\r' && data[i + 3] == '\n') {
                    end = data + i + 4;
                    break;
                }
            }
        }
        if (!end) {
            head.complete = false;
            return true;
        }
        head.complete = true;
        head.headerBytes = (size_t) (end - data);

        std::string_view buf(data, head.headerBytes);

        /* Status line: "HTTP/1.x <code> <reason>\r\n" */
        size_t lineEnd = buf.find("\r\n");
        std::string_view statusLine = buf.substr(0, lineEnd);
        if (statusLine.substr(0, 5) != "HTTP/") {
            return false;
        }
        size_t sp1 = statusLine.find(' ');
        if (sp1 == std::string_view::npos) {
            return false;
        }
        std::string_view rest = trim(statusLine.substr(sp1 + 1));
        size_t sp2 = rest.find(' ');
        std::string_view codeStr = (sp2 == std::string_view::npos) ? rest : rest.substr(0, sp2);
        if (codeStr.length() != 3) {
            return false;
        }
        int code = 0;
        for (char c : codeStr) {
            if (c < '0' || c > '9') {
                return false;
            }
            code = code * 10 + (c - '0');
        }
        head.status = code;
        head.reason = (sp2 == std::string_view::npos) ? std::string_view{} : trim(rest.substr(sp2 + 1));

        /* Headers */
        size_t pos = lineEnd + 2;
        while (pos < head.headerBytes) {
            size_t nl = buf.find("\r\n", pos);
            if (nl == std::string_view::npos || nl == pos) {
                break; /* empty line = end of headers */
            }
            std::string_view line = buf.substr(pos, nl - pos);
            size_t colon = line.find(':');
            if (colon != std::string_view::npos) {
                std::string_view name = trim(line.substr(0, colon));
                std::string_view value = trim(line.substr(colon + 1));
                std::string lower;
                lower.reserve(name.length());
                for (char c : name) {
                    lower += (char) tolower((unsigned char) c);
                }
                head.headers.emplace_back(std::move(lower), std::string(value));
            }
            pos = nl + 2;
        }
        return true;
    }

    /* Proxy CONNECT response parse (spec A.3b PROXY_CONNECT phase). The proxy replies with an ordinary
     * "HTTP/1.x <code> ..." status line followed by headers, so parseResponse handles it verbatim; this
     * named wrapper documents the reuse and that Proxy-Authenticate is reachable via findHeader() for
     * the failed.headers of a 407. Returns false only on a malformed status line; sets head.complete
     * false while the "\r\n\r\n" terminator has not yet arrived. */
    static bool parseProxyResponse(const char *data, size_t length, ResponseHead &head) {
        return parseResponse(data, length, head);
    }

    /* --- validators --- */

    /* Sec-WebSocket-Accept must equal SHA1/base64 of key + magic (WebSocketHandshake::generate). */
    static bool validateAccept(const char key[24], std::string_view accept) {
        char expected[29] = {};
        WebSocketHandshake::generate(key, expected);
        return accept.length() == 28 && memcmp(expected, accept.data(), 28) == 0;
    }

    /* The server-selected subprotocol (may be empty) must be one we offered. offeredCsv is the exact
     * comma-separated list we sent. */
    static bool validateProtocol(std::string_view offeredCsv, std::string_view chosen) {
        if (!chosen.length()) {
            return true;
        }
        size_t i = 0;
        while (i < offeredCsv.length()) {
            while (i < offeredCsv.length() && (offeredCsv[i] == ',' || offeredCsv[i] == ' ')) {
                i++;
            }
            size_t start = i;
            while (i < offeredCsv.length() && offeredCsv[i] != ',') {
                i++;
            }
            if (trim(offeredCsv.substr(start, i - start)) == chosen) {
                return true;
            }
        }
        return false;
    }

    /* --- client-side compression acceptance (mirror of negotiateCompression, spec A.3b) ---
     * server_max_window_bits / server_no_context_takeover bound OUR inflation window; client_* bound OUR
     * deflation window. Any parameter we did not offer, or an x-webkit-deflate-frame response, fails. */
    struct CompressionResult {
        bool ok = false;          /* false => fail the handshake */
        bool compression = false; /* did we end up with permessage-deflate on */
        CompressOptions options = DISABLED;
    };

    static CompressionResult acceptCompressionResponse(CompressOptions wanted, std::string_view ext) {
        /* We offered nothing: the server must not answer with any deflate extension. */
        if (wanted == DISABLED) {
            if (ext.length()) {
                ExtensionsParser ep(ext.data(), ext.length());
                if (ep.perMessageDeflate || ep.xWebKitDeflateFrame) {
                    return {false, false, DISABLED};
                }
            }
            return {true, false, DISABLED};
        }

        /* We offered permessage-deflate but the server declined (no extensions header). */
        if (!ext.length()) {
            return {true, false, DISABLED};
        }

        ExtensionsParser ep(ext.data(), ext.length());
        if (ep.xWebKitDeflateFrame) {
            return {false, false, DISABLED};
        }
        if (!ep.perMessageDeflate) {
            /* Server returned some other/garbage extension we never offered. */
            return {false, false, DISABLED};
        }

        int wantedInflationWindow = 0;
        if ((wanted & CompressOptions::_DECOMPRESSOR_MASK) != CompressOptions::SHARED_DECOMPRESSOR) {
            wantedInflationWindow = (wanted & CompressOptions::_DECOMPRESSOR_MASK) >> 8;
        }
        int wantedCompressionWindow = (wanted & CompressOptions::_COMPRESSOR_MASK) >> 4;

        bool offeredServerMaxBits = (wantedInflationWindow > 0 && wantedInflationWindow < 15);
        bool offeredClientMaxBits = (wantedCompressionWindow > 0);

        int inflationWindow = wantedInflationWindow;      /* 0 => shared decompressor */
        int compressionWindow = wantedCompressionWindow;  /* 0 => shared compressor */

        /* server_* bound our inflation. Per RFC 7692 the server MAY send *_no_context_takeover in the
         * response regardless of what we offered; it only reduces the state each side keeps, so always
         * honor it rather than treating it as an unoffered parameter. */
        if (ep.serverNoContextTakeover) {
            inflationWindow = 0;
        }
        if (ep.serverMaxWindowBits) {
            if (!offeredServerMaxBits) {
                return {false, false, DISABLED};
            }
            if (ep.serverMaxWindowBits != 1) { /* value present */
                if (ep.serverMaxWindowBits > wantedInflationWindow) {
                    return {false, false, DISABLED};
                }
                inflationWindow = ep.serverMaxWindowBits;
            }
        }

        /* client_* bound our deflation. client_no_context_takeover in the response is likewise always
         * valid (RFC 7692): the server is instructing us to reset our compressor per message. */
        if (ep.clientNoContextTakeover) {
            compressionWindow = 0;
        }
        if (ep.clientMaxWindowBits) {
            if (!offeredClientMaxBits) {
                return {false, false, DISABLED};
            }
            if (ep.clientMaxWindowBits != 1) {
                int upper = wantedCompressionWindow ? wantedCompressionWindow : 15;
                if (ep.clientMaxWindowBits > upper) {
                    return {false, false, DISABLED};
                }
                compressionWindow = ep.clientMaxWindowBits;
            }
        }

        /* Sanity (same shape as negotiateCompression's final check). */
        if ((compressionWindow && compressionWindow < 8) || compressionWindow > 15 ||
            (inflationWindow && inflationWindow < 8) || inflationWindow > 15) {
            return {false, false, DISABLED};
        }

        /* Map negotiated windows to CompressOptions exactly like HttpResponse::upgrade (:271-289). */
        CompressOptions options;
        if (compressionWindow == 0) {
            options = CompressOptions::SHARED_COMPRESSOR;
        } else {
            options = (CompressOptions) ((uint32_t) (compressionWindow << 4) | (uint32_t) (compressionWindow - 7));
            if (wanted & DEDICATED_COMPRESSOR_3KB) {
                options = DEDICATED_COMPRESSOR_3KB;
            }
        }
        if (inflationWindow == 0) {
            options = (CompressOptions) (options | CompressOptions::SHARED_DECOMPRESSOR);
        } else {
            options = (CompressOptions) (options | (inflationWindow << 8));
        }

        return {true, true, options};
    }
};

}

#endif // UWS_WEBSOCKETCLIENTHANDSHAKE_H
