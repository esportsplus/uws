/* We are only allowed to depend on µWS and V8 in this layer. */
#include "App.h"

#include <iostream>
#include <string>
#include <vector>
#include <type_traits>

#include <v8.h>
using namespace v8;

#include "Utilities.h"
#include "WebSocketWrapper.h"
#include "HttpResponseWrapper.h"
#include "HttpRequestWrapper.h"
#include "AppWrapper.h"

/* The outbound client layer, now bound to JS (step 10) via ClientAppWrapper.h. ClientApp.h must precede
 * ClientAppWrapper.h; ClientAppWrapper.h must follow AppWrapper.h (it reuses bindWebSocketBehavior and
 * readOptionsObject, which AppWrapper.h defines without an include guard). */
#include "ClientApp.h"
#include "ClientAppWrapper.h"

#include <functional>

/* Todo: Apps should be freed once the GC says so BUT ALWAYS before freeing the loop */

#include "Multipart.h"

/* This function is somewhat of a simplifying wrapper that does not follow the C++ library.
 * It takes a POST:ed body and contentType, and returns an array of parts if
 * the request is a multipart request */
void uWS_getParts(const FunctionCallbackInfo<Value> &args) {

    /* Because we mutate the strings, it is important that we get mutable input like
     * ArrayBuffer or Buffer, not String! */
    Isolate *isolate = args.GetIsolate();

    NativeString body(args.GetIsolate(), args[0]);
    if (body.isInvalid(args)) {
        return;
    }

    NativeString contentType(args.GetIsolate(), args[1]);
    if (contentType.isInvalid(args)) {
        return;
    }

    uWS::MultipartParser mp(contentType.getString());
    if (mp.isValid()) {
        mp.setBody(body.getString());

        std::pair<std::string_view, std::string_view> headers[MAX_HEADERS + 1];

        Local<Array> parts = Array::New(args.GetIsolate(), 0);

        while (true) {
            std::optional<std::string_view> optionalPart = mp.getNextPart(headers);
            if (!optionalPart.has_value()) {
                break;
            }

            std::string_view part = optionalPart.value();

            Local<ArrayBuffer> partArrayBuffer = ArrayBuffer_NewCopy(isolate, (void *) part.data(), part.length());
            /* Map is 30% faster in this case, but a static Object could be faster still */
            Local<Object> partMap = Object::New(isolate);
            partMap->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "data", NewStringType::kNormal).ToLocalChecked(), partArrayBuffer).IsNothing();

            for (int i = 0; headers[i].first.length(); i++) {
                /* We care about content-type and content-disposition */
                if (headers[i].first == "content-type") {
                    partMap->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "type", NewStringType::kNormal).ToLocalChecked(), String::NewFromUtf8(isolate, headers[i].second.data(), NewStringType::kNormal, headers[i].second.length()).ToLocalChecked()).IsNothing();
                } else if (headers[i].first == "content-disposition") {
                    /* Parse the parameters */
                    uWS::ParameterParser pp(headers[i].second);
                    while (true) {
                        auto [key, value] = pp.getKeyValue();
                        if (!key.length()) {
                            break;
                        }

                        // really anything that has both key and value and is not type or data?
                        if (key == "name" || key == "filename") {
                            partMap->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, key.data(), NewStringType::kNormal, key.length()).ToLocalChecked(), String::NewFromUtf8(isolate, value.data(), NewStringType::kNormal, value.length()).ToLocalChecked()).IsNothing();
                        }
                    }
                }
            }

            parts->Set(isolate->GetCurrentContext(), parts->Length(), partMap).IsNothing();
        }

        args.GetReturnValue().Set(parts);
    }

    /* We'll return undefined on error */
}

/* todo: Put this function and all inits of it in its own header */
bool uWS_is_listen_socket(const FunctionCallbackInfo<Value> &args, Local<Object> &listenSocket) {
    Isolate *isolate = args.GetIsolate();
    Local<Context> context = isolate->GetCurrentContext();
    Local<Private> marker = Private::ForApi(isolate, String::NewFromUtf8(isolate, "uWS.ListenSocket", NewStringType::kNormal).ToLocalChecked());

    if (args.Length() < 1 || !args[0]->IsObject()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "uWS: expected a uWS.ListenSocket", NewStringType::kNormal).ToLocalChecked()));
        return false;
    }

    listenSocket = args[0].As<Object>();
    Local<Value> isListenSocket;
    if (listenSocket->InternalFieldCount() != 1 || !listenSocket->GetPrivate(context, marker).ToLocal(&isListenSocket) || !isListenSocket->IsTrue()) {
        isolate->ThrowException(Exception::TypeError(String::NewFromUtf8(isolate, "uWS: expected a uWS.ListenSocket", NewStringType::kNormal).ToLocalChecked()));
        return false;
    }

    return true;
}

void uWS_us_listen_socket_close(const FunctionCallbackInfo<Value> &args) {
    Local<Object> listenSocket;
    if (!uWS_is_listen_socket(args, listenSocket)) {
        return;
    }

    struct us_listen_socket_t *socket = (struct us_listen_socket_t *) getInternalPointer(listenSocket);
    if (socket) {
        // this should take int ssl first
        us_listen_socket_close(0, socket);
        setInternalPointer(listenSocket, nullptr);
    }
}

void uWS_us_socket_local_port(const FunctionCallbackInfo<Value> &args) {
    Local<Object> listenSocket;
    if (!uWS_is_listen_socket(args, listenSocket)) {
        return;
    }

    struct us_listen_socket_t *socket = (struct us_listen_socket_t *) getInternalPointer(listenSocket);
    if (!socket) {
        args.GetIsolate()->ThrowException(Exception::Error(String::NewFromUtf8(args.GetIsolate(), "uWS: listen socket is closed", NewStringType::kNormal).ToLocalChecked()));
        return;
    }

    // this should take int ssl first, but us_socket_local_port doesn't use it so it doesn't matter
    int port = us_socket_local_port(0, (struct us_socket_t *) socket);
    args.GetReturnValue().Set(Integer::New(args.GetIsolate(), port));
}

PerContextData *Main(Isolate *isolate, Local<Object> exports) {

    /* Init the template objects, SSL and non-SSL, store it in per context data */
    PerContextData *perContextData = new PerContextData;
    perContextData->isolate = isolate;
    perContextData->reqTemplate.Reset(isolate, HttpRequestWrapper::init(isolate));
    perContextData->resTemplate[0].Reset(isolate, HttpResponseWrapper::init<0>(isolate));
    perContextData->resTemplate[1].Reset(isolate, HttpResponseWrapper::init<1>(isolate));
    perContextData->wsTemplate[0].Reset(isolate, WebSocketWrapper::init<0, 1>(isolate));
    perContextData->wsTemplate[1].Reset(isolate, WebSocketWrapper::init<1, 1>(isolate));
    /* Outbound client ws objects (isServer = false): distinct class names, cast to WebSocket<SSL, false>. */
    perContextData->clientWsTemplate[0].Reset(isolate, WebSocketWrapper::init<0, 0>(isolate));
    perContextData->clientWsTemplate[1].Reset(isolate, WebSocketWrapper::init<1, 0>(isolate));

    /* Refer to per context data via External */
    Local<External> externalPerContextData = External::New(isolate, perContextData);

    /* uWS namespace */
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "App", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App<uWS::App>, externalPerContextData)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "SSLApp", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App<uWS::SSLApp>, externalPerContextData)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();

    /* Outbound WebSocket clients: Client dials ws://, SSLClient dials wss:// */
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "Client", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Client<uWS::ClientApp>, externalPerContextData)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "SSLClient", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_Client<uWS::SSLClientApp>, externalPerContextData)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();

    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "getParts", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_getParts)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    
    /* Expose some µSockets functions directly under uWS namespace */
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "us_listen_socket_close", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_us_listen_socket_close)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "us_socket_local_port", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_us_socket_local_port)->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()).ToChecked();

    /* Compression enum */
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DISABLED", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DISABLED)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "SHARED_COMPRESSOR", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::SHARED_COMPRESSOR)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "SHARED_DECOMPRESSOR", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::SHARED_DECOMPRESSOR)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_DECOMPRESSOR", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_DECOMPRESSOR)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_COMPRESSOR", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_COMPRESSOR)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_COMPRESSOR_3KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_COMPRESSOR_3KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_COMPRESSOR_4KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_COMPRESSOR_4KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_COMPRESSOR_8KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_COMPRESSOR_8KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_COMPRESSOR_16KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_COMPRESSOR_16KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_COMPRESSOR_32KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_COMPRESSOR_32KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_COMPRESSOR_64KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_COMPRESSOR_64KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_COMPRESSOR_128KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_COMPRESSOR_128KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_COMPRESSOR_256KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_COMPRESSOR_256KB)).ToChecked();

    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_DECOMPRESSOR_32KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_DECOMPRESSOR_32KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_DECOMPRESSOR_16KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_DECOMPRESSOR_16KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_DECOMPRESSOR_8KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_DECOMPRESSOR_8KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_DECOMPRESSOR_4KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_DECOMPRESSOR_4KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_DECOMPRESSOR_2KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_DECOMPRESSOR_2KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_DECOMPRESSOR_1KB", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_DECOMPRESSOR_1KB)).ToChecked();
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "DEDICATED_DECOMPRESSOR_512B", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, uWS::DEDICATED_DECOMPRESSOR_512B)).ToChecked();

    /* Listen options */
    exports->Set(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "LIBUS_LISTEN_EXCLUSIVE_PORT", NewStringType::kNormal).ToLocalChecked(), Integer::NewFromUnsigned(isolate, LIBUS_LISTEN_EXCLUSIVE_PORT)).ToChecked();

    return perContextData;
}

/* This is required when building as a Node.js addon */
#ifndef ADDON_IS_HOST
#include <node.h>
extern "C" NODE_MODULE_EXPORT void
NODE_MODULE_INITIALIZER(Local<Object> exports, Local<Value> module, Local<Context> context) {
    Isolate *isolate = Isolate::GetCurrent();
    /* Integrate uSockets with existing libuv loop */
    uWS::Loop::get(node::GetCurrentEventLoop(isolate));
    /* Register vanilla V8 addon */
    PerContextData *perContextData = Main(isolate, exports);

    /* We cannot rely on process.exit or process.beforeExit when it comes to WorkerThreads */
    node::AddEnvironmentCleanupHook(isolate, [](void *arg, void (*done)(void *), void *doneArg) {

        PerContextData *perContextData = (PerContextData *) arg;

        /* Free client apps FIRST: their destructors cancel pending connect requests (DNS + ms timer
         * handles) and close per-connect contexts, which must happen before the server apps and the
         * loop are torn down. */
        perContextData->clients.clear();
        perContextData->sslClients.clear();
        /* Freeing apps here, it could be done earlier but not sooner */
        perContextData->apps.clear();
        perContextData->sslApps.clear();

        /* Orphaned getaddrinfo requests may outlive their context after uv_cancel. Reap only those
         * callbacks before freeing our loop data; this must not be widened to all libuv activity. */
        us_loop_t *loop = (us_loop_t *) uWS::Loop::get();
        uv_loop_t *nativeLoop = node::GetCurrentEventLoop(perContextData->isolate);
        while (us_internal_loop_pending_resolves(loop) > 0) {
            us_loop_run_once(loop);
        }

        /* Free the loop only after all orphaned resolver callbacks have retired. */
        uWS::Loop::get()->free();

        /* We can safely delete this since we no longer can call uWS.free */
        delete perContextData;

        /* Node must keep the addon loaded until libuv has invoked its deferred close callbacks.
         * In particular, Windows unloads the DLL before draining the worker loop after a synchronous
         * cleanup hook. Wait only for closing handles; unrelated active handles do not block us. */
        struct Cleanup {
            uv_timer_t timer;
            void (*done)(void *);
            void *arg;
        };
        auto *cleanup = new Cleanup{};
        cleanup->done = done;
        cleanup->arg = doneArg;
        uv_timer_init(nativeLoop, &cleanup->timer);
        cleanup->timer.data = cleanup;
        uv_timer_start(&cleanup->timer, [](uv_timer_t *timer) {
            bool closing = false;
            uv_walk(timer->loop, [](uv_handle_t *handle, void *arg) {
                if (uv_is_closing(handle)) {
                    *static_cast<bool *>(arg) = true;
                }
            }, &closing);
            if (!closing) {
                uv_timer_stop(timer);
                uv_close((uv_handle_t *) timer, [](uv_handle_t *handle) {
                    auto *cleanup = static_cast<Cleanup *>(handle->data);
                    auto done = cleanup->done;
                    void *arg = cleanup->arg;
                    delete cleanup;
                    done(arg);
                });
            }
        }, 0, 1);

    }, perContextData);
}
#endif
