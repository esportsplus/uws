#ifndef ADDON_CLIENTAPPWRAPPER_H
#define ADDON_CLIENTAPPWRAPPER_H

/* V8 binding for the outbound WebSocket client role (spec A.3c). Mirrors AppWrapper.h:
 *   uWS_Client<CLIENTAPP>            -> the Client() / SSLClient() factory
 *   uWS_Client_connect<CLIENTAPP>    -> client.connect(url, behavior)
 *   uWS_Client_close / _publish / _numSubscribers
 *
 * MUST be included AFTER AppWrapper.h: it reuses bindWebSocketBehavior() (the shared open/message/...
 * lambda binder) and readOptionsObject() from there. AppWrapper.h has no include guard, so this header
 * does not include it (that would redefine those templates); addon.cpp controls the order. */

#include "ClientApp.h"
#include "Utilities.h"

#include <charconv>
#include <algorithm>
#include <string>
#include <string_view>
#include <openssl/evp.h>
#include <v8.h>
/* libuv, bundled with Node, for uv_err_name: refine libuv EAI_* resolver codes to precise names. */
#include <uv.h>
using namespace v8;

namespace {

/* Percent-decode `in` into `out`. Returns false on a malformed %XX escape. */
inline bool proxyPercentDecode(std::string_view in, std::string &out) {
    out.clear();
    out.reserve(in.size());
    auto hexVal = [](char h, int &v) -> bool {
        if (h >= '0' && h <= '9') { v = h - '0'; return true; }
        if (h >= 'a' && h <= 'f') { v = h - 'a' + 10; return true; }
        if (h >= 'A' && h <= 'F') { v = h - 'A' + 10; return true; }
        return false;
    };
    for (size_t i = 0; i < in.size(); i++) {
        if (in[i] == '%') {
            int hi = 0, lo = 0;
            if (i + 2 >= in.size() || !hexVal(in[i + 1], hi) || !hexVal(in[i + 2], lo)) {
                return false;
            }
            out.push_back((char) ((hi << 4) | lo));
            i += 2;
        } else {
            out.push_back(in[i]);
        }
    }
    return true;
}

/* Base64-encode via BoringSSL EVP_EncodeBlock (used for the Proxy-Authorization credential). */
inline std::string proxyBase64(std::string_view data) {
    if (data.empty()) {
        return std::string();
    }
    std::string out(4 * ((data.size() + 2) / 3), '\0');
    int n = EVP_EncodeBlock((unsigned char *) out.data(), (const unsigned char *) data.data(), (int) data.size());
    out.resize(n < 0 ? 0 : (size_t) n);
    return out;
}

/* Parse an HTTP CONNECT proxy URL: http://[user:pass@]host:port (spec A.3b). Fills `cfg` and returns
 * true; returns false (-> URL_INVALID) on a non-http scheme, a missing port, or malformed userinfo.
 * userinfo is percent-decoded per component and base64-encoded into a ready "Basic ..." authHeader. */
inline bool proxyParseUrl(std::string_view url, uWS::ProxyConfig &cfg) {
    size_t schemeEnd = url.find("://");
    if (schemeEnd == std::string_view::npos || url.substr(0, schemeEnd) != "http") {
        return false;
    }
    std::string_view rest = url.substr(schemeEnd + 3);
    size_t authEnd = rest.find_first_of("/?");
    std::string_view authority = (authEnd == std::string_view::npos) ? rest : rest.substr(0, authEnd);

    std::string_view userinfo;
    size_t at = authority.rfind('@');
    if (at != std::string_view::npos) {
        userinfo = authority.substr(0, at);
        authority = authority.substr(at + 1);
    }

    std::string host;
    std::string_view portStr;
    if (authority.length() && authority.front() == '[') {
        size_t close = authority.find(']');
        if (close == std::string_view::npos) {
            return false;
        }
        host.assign(authority.substr(1, close - 1));
        std::string_view after = authority.substr(close + 1);
        if (after.empty() || after.front() != ':') {
            return false;
        }
        portStr = after.substr(1);
    } else {
        size_t colon = authority.rfind(':');
        if (colon == std::string_view::npos) {
            return false;
        }
        host.assign(authority.substr(0, colon));
        portStr = authority.substr(colon + 1);
    }
    if (host.empty() || portStr.empty()) {
        return false;
    }

    int port = 0;
    auto r = std::from_chars(portStr.data(), portStr.data() + portStr.length(), port);
    if (r.ec != std::errc() || r.ptr != portStr.data() + portStr.length() || port <= 0 || port > 65535) {
        return false;
    }

    cfg.enabled = true;
    cfg.host = std::move(host);
    cfg.port = port;
    cfg.authHeader.clear();
    if (userinfo.length()) {
        std::string user, pass;
        size_t colon = userinfo.find(':');
        if (colon == std::string_view::npos) {
            if (!proxyPercentDecode(userinfo, user)) {
                return false;
            }
        } else {
            if (!proxyPercentDecode(userinfo.substr(0, colon), user) ||
                !proxyPercentDecode(userinfo.substr(colon + 1), pass)) {
                return false;
            }
        }
        cfg.authHeader = "Basic " + proxyBase64(user + ":" + pass);
    }
    return true;
}

}

/* client.connect(url, behavior) -> this (spec A.3c uWS_Client_connect). Throws synchronously on an
 * invalid URL / scheme / header (surfaced by ClientApp::connect as std::runtime_error); every other
 * failure is delivered asynchronously to behavior.failed. */
template <typename CLIENTAPP>
void uWS_Client_connect(const FunctionCallbackInfo<Value> &args) {
    constexpr bool SSL = std::is_same<CLIENTAPP, uWS::SSLClientApp>::value;
    Isolate *isolate = args.GetIsolate();
    Local<Context> context = isolate->GetCurrentContext();
    CLIENTAPP *app = (CLIENTAPP *) getInternalPointer(args.This());
    PerContextData *perContextData = (PerContextData *) Local<External>::Cast(args.Data())->Value();

    if (!app) {
        args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::Error(String::NewFromUtf8(isolate, "CLIENT_CLOSED", NewStringType::kNormal).ToLocalChecked())));
        return;
    }

    /* url, behavior */
    if (missingArguments(2, args)) {
        return;
    }

    NativeString url(isolate, args[0]);
    if (url.isInvalid(args)) {
        return;
    }
    std::string urlString(url.getString());

    if (!args[1]->IsObject()) {
        args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::Error(String::NewFromUtf8(isolate, "uWS: connect(url, behavior) requires a behavior object.", NewStringType::kNormal).ToLocalChecked())));
        return;
    }
    Local<Object> behaviorObject = Local<Object>::Cast(args[1]);

    typename CLIENTAPP::template ConnectBehavior<PerSocketData> behavior = {};

    /* Preserve exceptions from behavior property getters for the caller. */
    auto getVal = [&](const char *name) -> MaybeLocal<Value> {
        return behaviorObject->Get(context, String::NewFromUtf8(isolate, name, NewStringType::kNormal).ToLocalChecked());
    };

    /* --- shared frame-phase settings (mirror uWS_App_ws; ClientApp::connect clamps ranges) --- */
    {
        Local<Value> v;
        if (!getVal("maxPayloadLength").ToLocal(&v)) { return; }
        if (!v->IsUndefined() && !requireInt32(args, v, &behavior.maxPayloadLength)) return;
    }
    {
        Local<Value> v;
        if (!getVal("idleTimeout").ToLocal(&v)) { return; }
        if (!v->IsUndefined() && !requireInt32(args, v, &behavior.idleTimeout)) return;
    }
    {
        Local<Value> v;
        if (!getVal("maxLifetime").ToLocal(&v)) { return; }
        if (!v->IsUndefined() && !requireInt32(args, v, &behavior.maxLifetime)) return;
    }
    {
        Local<Value> v;
        if (!getVal("maxBackpressure").ToLocal(&v)) { return; }
        if (!v->IsUndefined() && !requireInt32(args, v, &behavior.maxBackpressure)) return;
    }
    {
        Local<Value> v;
        if (!getVal("closeOnBackpressureLimit").ToLocal(&v)) { return; }
        if (!v->IsUndefined()) { behavior.closeOnBackpressureLimit = v->BooleanValue(isolate); }
    }
    {
        Local<Value> v;
        if (!getVal("sendPingsAutomatically").ToLocal(&v)) { return; }
        if (!v->IsUndefined()) { behavior.sendPingsAutomatically = v->BooleanValue(isolate); }
    }
    {
        Local<Value> v;
        if (!getVal("compression").ToLocal(&v)) { return; }
        if (!v->IsUndefined()) { int compression; if (!requireInt32(args, v, &compression)) return; behavior.compression = (uWS::CompressOptions) compression; }
    }

    /* --- client-specific settings --- */
    {
        Local<Value> v;
        if (!getVal("connectTimeout").ToLocal(&v)) { return; }
        if (!v->IsUndefined() && !requireInt32(args, v, &behavior.connectTimeout)) return;
    }
    {
        /* Milliseconds. */
        Local<Value> v;
        if (!getVal("handshakeTimeout").ToLocal(&v)) { return; }
        if (!v->IsUndefined() && !requireInt32(args, v, &behavior.handshakeTimeout)) return;
    }
    {
        Local<Value> v;
        if (!getVal("rejectUnauthorized").ToLocal(&v)) { return; }
        if (!v->IsUndefined()) { behavior.rejectUnauthorized = v->BooleanValue(isolate) ? 1 : 0; }
    }
    {
        Local<Value> v;
        if (!getVal("servername").ToLocal(&v)) { return; }
        NativeString servername(isolate, v);
        if (servername.isInvalid(args)) { return; }
        if (servername.getString().length()) { behavior.servername = servername.getString(); }
    }
    {
        Local<Value> v;
        if (!getVal("sourceHost").ToLocal(&v)) { return; }
        NativeString sourceHost(isolate, v);
        if (sourceHost.isInvalid(args)) { return; }
        if (sourceHost.getString().length()) { behavior.sourceHost = sourceHost.getString(); }
    }
    {
        Local<Value> v;
        if (!getVal("protocols").ToLocal(&v)) { return; }
        if (v->IsArray()) {
            Local<Array> arr = Local<Array>::Cast(v);
            for (uint32_t i = 0; i < arr->Length(); i++) {
                Local<Value> protocol;
                if (!arr->Get(context, i).ToLocal(&protocol)) { return; }
                NativeString p(isolate, protocol);
                if (p.isInvalid(args)) { return; }
                behavior.protocols.emplace_back(p.getString());
            }
        }
    }
    {
        Local<Value> v;
        if (!getVal("headers").ToLocal(&v)) { return; }
        if (v->IsObject() && !v->IsArray()) {
            Local<Object> headersObject = Local<Object>::Cast(v);
            Local<Array> names;
            if (!headersObject->GetOwnPropertyNames(context).ToLocal(&names)) { return; }
            for (uint32_t i = 0; i < names->Length(); i++) {
                Local<Value> keyVal;
                if (!names->Get(context, i).ToLocal(&keyVal)) { return; }
                NativeString k(isolate, keyVal);
                if (k.isInvalid(args)) { return; }
                Local<Value> headerValue;
                if (!headersObject->Get(context, keyVal).ToLocal(&headerValue)) { return; }
                NativeString val(isolate, headerValue);
                if (val.isInvalid(args)) { return; }
                behavior.headers.emplace_back(std::string(k.getString()), std::string(val.getString()));
            }
        }
    }

    /* --- proxy: an explicit per-connect proxy overrides the Client default; '' disables it; absent
     * inherits the Client default. A bad proxy URL throws URL_INVALID synchronously (before pcd is
     * allocated, so nothing leaks). --- */
    {
        Local<Value> v;
        if (!getVal("proxy").ToLocal(&v)) { return; }
        if (v->IsUndefined()) {
            behavior.proxy = app->defaultProxy;
        } else {
            NativeString proxy(isolate, v);
            if (proxy.isInvalid(args)) { return; }
            if (proxy.getString().length() && !proxyParseUrl(proxy.getString(), behavior.proxy)) {
                args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::Error(String::NewFromUtf8(isolate, "URL_INVALID", NewStringType::kNormal).ToLocalChecked())));
                return;
            }
        }
    }

    /* --- userData: allocate a heap PerSocketData holding the persistent, exactly the slot the server's
     * res.upgrade fills. Ownership passes into the native connect on success (moved into the socket at
     * open by adoptToFrame) or is released in emitFailed on failure. If connect() throws synchronously,
     * the catch below frees it (connect throws before storing it). --- */
    PerSocketData *pcd = nullptr;
    {
        Local<Value> v;
        if (!getVal("userData").ToLocal(&v)) { return; }
        if (v->IsObject()) {
            pcd = new PerSocketData();
            pcd->socketPf.Reset(isolate, Local<Object>::Cast(v));
        }
    }
    behavior.userData = pcd;

    /* --- failed handler (client-specific; delivers a ConnectError object) --- */
    {
        UniquePersistent<Function> failedPf;
        Local<Value> v;
        if (!getVal("failed").ToLocal(&v)) { delete pcd; return; }
        if (!v->IsUndefined() && !requireFunction(args, v)) {
            delete pcd;
            return;
        }
        if (v->IsFunction()) {
            failedPf.Reset(isolate, Local<Function>::Cast(v));
        }
        behavior.failed = [failedPf = std::move(failedPf), perContextData](uWS::ClientConnectError error) {
            Isolate *isolate = perContextData->isolate;
            HandleScope hs(isolate);
            Local<Function> failedLf = Local<Function>::New(isolate, failedPf);
            if (failedLf.IsEmpty() || failedLf->IsUndefined()) {
                return;
            }
            Local<Context> ctx = isolate->GetCurrentContext();
            Local<Object> errObject = Object::New(isolate);

            /* Refine libuv EAI_* resolver codes (negative) to precise names. On Unix, libuv system
             * errors are negated errnos, so positive µWS errnos can be refined precisely too. */
            const char *codeStr = error.code;
#ifndef _WIN32
            char buf[64];
#endif
            if (error.errnoCode < 0) {
                codeStr = uv_err_name(error.errnoCode);
            }
#ifndef _WIN32
            else if (error.errnoCode > 0) {
                uv_err_name_r(-error.errnoCode, buf, sizeof(buf));
                codeStr = buf;
            }
#endif
            errObject->Set(ctx, String::NewFromUtf8(isolate, "code", NewStringType::kNormal).ToLocalChecked(),
                String::NewFromUtf8(isolate, codeStr, NewStringType::kNormal).ToLocalChecked()).Check();
            errObject->Set(ctx, String::NewFromUtf8(isolate, "message", NewStringType::kNormal).ToLocalChecked(),
                String::NewFromUtf8(isolate, error.message.data(), NewStringType::kNormal, (int) error.message.length()).ToLocalChecked()).Check();
            if (error.status) {
                errObject->Set(ctx, String::NewFromUtf8(isolate, "status", NewStringType::kNormal).ToLocalChecked(),
                    Integer::New(isolate, error.status)).Check();
            }
            if (error.headers) {
                Local<Object> headersObject = Object::New(isolate);
                for (auto &kv : *error.headers) {
                    headersObject->Set(ctx, String::NewFromUtf8(isolate, kv.first.data(), NewStringType::kNormal, (int) kv.first.length()).ToLocalChecked(),
                        String::NewFromUtf8(isolate, kv.second.data(), NewStringType::kNormal, (int) kv.second.length()).ToLocalChecked()).Check();
                }
                errObject->Set(ctx, String::NewFromUtf8(isolate, "headers", NewStringType::kNormal).ToLocalChecked(), headersObject).Check();
            }

            Local<Value> argv[1] = {errObject};
            CallJS(isolate, failedLf, 1, argv);
        };
    }

    /* --- shared handler lambdas (open/message/drain/close/dropped/ping/pong/subscription), bound the
     * same way as the server so the contract cannot drift. The client ws objects are cloned from
     * clientWsTemplate[SSL]. --- */
    UniquePersistent<Function> openPf, messagePf, drainPf, closePf, droppedPf, pingPf, pongPf, subscriptionPf;
    auto readClientFunction = [&](const char *name, UniquePersistent<Function> &persistent) {
        Local<Value> value;
        if (!getVal(name).ToLocal(&value)) return false;
        if (!value->IsUndefined() && !requireFunction(args, value)) return false;
        persistent.Reset(isolate, Local<Function>::Cast(value));
        return true;
    };
    if (!readClientFunction("open", openPf) || !readClientFunction("message", messagePf) ||
        !readClientFunction("drain", drainPf) || !readClientFunction("close", closePf) ||
        !readClientFunction("dropped", droppedPf) || !readClientFunction("ping", pingPf) ||
        !readClientFunction("pong", pongPf) || !readClientFunction("subscription", subscriptionPf)) {
        delete pcd;
        return;
    }

    bindWebSocketBehavior(isolate, perContextData, &perContextData->clientWsTemplate[SSL ? 1 : 0], behavior,
        std::move(openPf), std::move(messagePf), std::move(drainPf), std::move(closePf),
        std::move(droppedPf), std::move(pingPf), std::move(pongPf), std::move(subscriptionPf));

    /* --- dial --- */
    try {
        app->template connect<PerSocketData>(urlString, std::move(behavior));
    } catch (const std::exception &e) {
        /* Synchronous failure (URL_INVALID / HEADER_INVALID) before any I/O: connect() has not stored
         * the userData, so free it here to avoid a leak. */
        if (pcd) {
            delete pcd;
        }
        args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::Error(String::NewFromUtf8(isolate, e.what(), NewStringType::kNormal).ToLocalChecked())));
        return;
    }

    args.GetReturnValue().Set(args.This());
}

/* client.close() -> this. Cancels pending connects and closes open sockets synchronously, then frees
 * the client on the next loop tick after any current socket callback has unwound. */
template <typename CLIENTAPP>
void uWS_Client_close(const FunctionCallbackInfo<Value> &args) {
    CLIENTAPP *app = (CLIENTAPP *) getInternalPointer(args.This());
    if (!app) {
        args.GetReturnValue().Set(args.This());
        return;
    }

    app->close();
    setInternalPointer(args.This(), nullptr);

    PerContextData *perContextData = (PerContextData *) Local<External>::Cast(args.Data())->Value();
    uWS::Loop::get()->defer([perContextData, app]() {
        if constexpr (std::is_same<CLIENTAPP, uWS::SSLClientApp>::value) {
            auto it = std::find_if(perContextData->sslClients.begin(), perContextData->sslClients.end(),
                [app](const auto &p) { return p.get() == app; });
            if (it != perContextData->sslClients.end()) {
                perContextData->sslClients.erase(it);
            }
        } else {
            auto it = std::find_if(perContextData->clients.begin(), perContextData->clients.end(),
                [app](const auto &p) { return p.get() == app; });
            if (it != perContextData->clients.end()) {
                perContextData->clients.erase(it);
            }
        }
    });
    args.GetReturnValue().Set(args.This());
}

/* client.publish(topic, message, isBinary?, compress?) -> bool */
template <typename CLIENTAPP>
void uWS_Client_publish(const FunctionCallbackInfo<Value> &args) {
    Isolate *isolate = args.GetIsolate();
    CLIENTAPP *app = (CLIENTAPP *) getInternalPointer(args.This());

    if (!app) {
        args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::Error(String::NewFromUtf8(isolate, "CLIENT_CLOSED", NewStringType::kNormal).ToLocalChecked())));
        return;
    }

    if (missingArguments(2, args)) {
        return;
    }

    NativeString topic(isolate, args[0]);
    if (topic.isInvalid(args)) {
        return;
    }
    NativeString message(isolate, args[1]);
    if (message.isInvalid(args)) {
        return;
    }

    bool ok = app->publish(topic.getString(), message.getString(),
        args[2]->BooleanValue(isolate) ? uWS::OpCode::BINARY : uWS::OpCode::TEXT, args[3]->BooleanValue(isolate));
    args.GetReturnValue().Set(Boolean::New(isolate, ok));
}

/* client.numSubscribers(topic) -> integer */
template <typename CLIENTAPP>
void uWS_Client_numSubscribers(const FunctionCallbackInfo<Value> &args) {
    Isolate *isolate = args.GetIsolate();
    CLIENTAPP *app = (CLIENTAPP *) getInternalPointer(args.This());

    if (!app) {
        args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::Error(String::NewFromUtf8(isolate, "CLIENT_CLOSED", NewStringType::kNormal).ToLocalChecked())));
        return;
    }

    if (missingArguments(1, args)) {
        return;
    }
    NativeString topic(isolate, args[0]);
    if (topic.isInvalid(args)) {
        return;
    }
    args.GetReturnValue().Set(Integer::New(isolate, app->numSubscribers(topic.getString())));
}

/* Client(options?) / SSLClient(options) factory (spec A.3c). Mirrors uWS_App. */
template <typename CLIENTAPP>
void uWS_Client(const FunctionCallbackInfo<Value> &args) {
    Isolate *isolate = args.GetIsolate();
    Local<Context> context = isolate->GetCurrentContext();

    auto [options, valid] = readOptionsObject(args, 0);
    if (!valid) {
        return;
    }

    CLIENTAPP *app = new CLIENTAPP(options);
    if (app->constructorFailed()) {
        delete app;
        args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::Error(String::NewFromUtf8(isolate, "Client construction failed", NewStringType::kNormal).ToLocalChecked())));
        return;
    }

    /* Default proxy from ClientOptions.proxy (parsed once; applied to any connect that omits its own).
     * readOptionsObject ignores it (not a SocketContextOptions field), so read it here. */
    if (args.Length() > 0 && args[0]->IsObject()) {
        Local<Object> optionsObject = Local<Object>::Cast(args[0]);
        Local<Value> proxyValue;
        if (!optionsObject->Get(context, String::NewFromUtf8(isolate, "proxy", NewStringType::kNormal).ToLocalChecked()).ToLocal(&proxyValue)) {
            delete app;
            return;
        }
        NativeString proxy(isolate, proxyValue);
        if (proxy.isInvalid(args)) {
            delete app;
            return;
        }
        if (proxy.getString().length() && !proxyParseUrl(proxy.getString(), app->defaultProxy)) {
            delete app;
            args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::Error(String::NewFromUtf8(isolate, "URL_INVALID", NewStringType::kNormal).ToLocalChecked())));
            return;
        }
    }

    PerContextData *perContextData = (PerContextData *) Local<External>::Cast(args.Data())->Value();
    constexpr int clientTypeIndex = getClientAppTypeIndex<CLIENTAPP>();

    if (perContextData->clientAppTemplate[clientTypeIndex].IsEmpty()) {
        Local<FunctionTemplate> clientTemplate = FunctionTemplate::New(isolate);
        clientTemplate->SetClassName(String::NewFromUtf8(isolate, std::is_same<CLIENTAPP, uWS::SSLClientApp>::value ? "uWS.SSLClient" : "uWS.Client", NewStringType::kNormal).ToLocalChecked());
        clientTemplate->InstanceTemplate()->SetInternalFieldCount(1);

        clientTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "connect", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Client_connect<CLIENTAPP>, args.Data()));
        clientTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "close", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Client_close<CLIENTAPP>, args.Data()));
        clientTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "publish", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Client_publish<CLIENTAPP>, args.Data()));
        clientTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "numSubscribers", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Client_numSubscribers<CLIENTAPP>, args.Data()));

        perContextData->clientAppTemplate[clientTypeIndex].Reset(isolate, clientTemplate);
    }

    Local<FunctionTemplate> clientTemplate = perContextData->clientAppTemplate[clientTypeIndex].Get(isolate);
    Local<Object> localClient = clientTemplate->GetFunction(context).ToLocalChecked()->NewInstance(context).ToLocalChecked();
    setInternalPointer(localClient, app);

    /* Held until env cleanup; freed BEFORE the server apps and the loop (see addon.cpp). */
    if constexpr (std::is_same<CLIENTAPP, uWS::SSLClientApp>::value) {
        perContextData->sslClients.emplace_back(app);
    } else {
        perContextData->clients.emplace_back(app);
    }

    args.GetReturnValue().Set(localClient);
}

#endif // ADDON_CLIENTAPPWRAPPER_H
