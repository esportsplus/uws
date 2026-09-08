#include "App.h"
#include <v8.h>
#include "Utilities.h"
using namespace v8;

thread_local int insideRouteDispatch = 0;

/* Installs the shared WebSocket handler lambdas (open, message, dropped, drain, subscription,
 * ping, pong, close) onto `behavior` from already-read persistent callbacks. Shared by the
 * server App.ws path and the outbound client connect path so the handler contract cannot drift.
 * The `upgrade` handler is server-only and bound by the caller. `wsTemplate` is the per-context
 * template used to clone the JS WebSocket object in the open handler (server: wsTemplate[SSL],
 * client: clientWsTemplate[SSL]). */
template <typename Behavior>
void bindWebSocketBehavior(Isolate *isolate, PerContextData *perContextData,
    UniquePersistent<Object> *wsTemplate, Behavior &behavior,
    UniquePersistent<Function> openPf,
    UniquePersistent<Function> messagePf,
    UniquePersistent<Function> drainPf,
    UniquePersistent<Function> closePf,
    UniquePersistent<Function> droppedPf,
    UniquePersistent<Function> pingPf,
    UniquePersistent<Function> pongPf,
    UniquePersistent<Function> subscriptionPf) {

    /* Open handler is NOT optional for the wrapper */
    behavior.open = [openPf = std::move(openPf), perContextData, wsTemplate](auto *ws) {
        Isolate *isolate = perContextData->isolate;
        HandleScope hs(isolate);

        /* Create a new websocket object */
        Local<Object> wsObject = wsTemplate->Get(isolate)->Clone();
        setInternalPointer(wsObject, ws);

        /* Retrieve temporary userData object */
        PerSocketData *perSocketData = (PerSocketData *) ws->getUserData();

        /* Store the upgrade-time userData object in the ws object's second internal field, read
         * back via getUserData(). Default to an empty object when no userData was provided. */
        if (!perSocketData->socketPf.IsEmpty()) {
            /* socketPf points to a stack allocated UniquePersistent, or nullptr, at this point */
            wsObject->SetInternalField(1, Local<Object>::New(isolate, perSocketData->socketPf));
        } else {
            wsObject->SetInternalField(1, Object::New(isolate));
        }

        /* Attach a new V8 object with pointer to us, to it */
        perSocketData->socketPf.Reset(isolate, wsObject);

        Local<Function> openLf = Local<Function>::New(isolate, openPf);
        if (!openLf->IsUndefined()) {
            Local<Value> argv[] = {wsObject};
            CallJS(isolate, openLf, 1, argv);
        }
    };

    /* Message handler is always optional */
    if (messagePf != Undefined(isolate)) {
        behavior.message = [messagePf = std::move(messagePf), isolate](auto *ws, std::string_view message, uWS::OpCode opCode) {
            HandleScope hs(isolate);

            Local<ArrayBuffer> messageArrayBuffer = ArrayBuffer_New(isolate, (void *) message.data(), message.length());

            PerSocketData *perSocketData = (PerSocketData *) ws->getUserData();
            Local<Value> argv[3] = {Local<Object>::New(isolate, perSocketData->socketPf),
                                    messageArrayBuffer,
                                    Boolean::New(isolate, opCode == uWS::OpCode::BINARY)};

            CallJS(isolate, Local<Function>::New(isolate, messagePf), 3, argv);

            /* Important: we clear the ArrayBuffer to make sure it is not invalidly used after return */
            messageArrayBuffer->Detach();
        };
    }

    /* Dropped handler is always optional (similar to message) */
    if (droppedPf != Undefined(isolate)) {
        behavior.dropped = [droppedPf = std::move(droppedPf), isolate](auto *ws, std::string_view message, uWS::OpCode opCode) {
            HandleScope hs(isolate);

            Local<ArrayBuffer> messageArrayBuffer = ArrayBuffer_New(isolate, (void *) message.data(), message.length());

            PerSocketData *perSocketData = (PerSocketData *) ws->getUserData();
            Local<Value> argv[3] = {Local<Object>::New(isolate, perSocketData->socketPf),
                                    messageArrayBuffer,
                                    Boolean::New(isolate, opCode == uWS::OpCode::BINARY)};

            CallJS(isolate, Local<Function>::New(isolate, droppedPf), 3, argv);

            /* Important: we clear the ArrayBuffer to make sure it is not invalidly used after return */
            messageArrayBuffer->Detach();
        };
    }

    /* Drain handler is always optional */
    if (drainPf != Undefined(isolate)) {
        behavior.drain = [drainPf = std::move(drainPf), isolate](auto *ws) {
            HandleScope hs(isolate);

            PerSocketData *perSocketData = (PerSocketData *) ws->getUserData();
            Local<Value> argv[1] = {Local<Object>::New(isolate, perSocketData->socketPf)
                                    };
            CallJS(isolate, Local<Function>::New(isolate, drainPf), 1, argv);
        };
    }

    /* Subscription handler is always optional */
    if (subscriptionPf != Undefined(isolate)) {
        behavior.subscription = [subscriptionPf = std::move(subscriptionPf), isolate](auto *ws, std::string_view topic, int newCount, int oldCount) {
            HandleScope hs(isolate);

            Local<ArrayBuffer> topicArrayBuffer = ArrayBuffer_New(isolate, (void *) topic.data(), topic.length());
            PerSocketData *perSocketData = (PerSocketData *) ws->getUserData();
            Local<Value> argv[4] = {Local<Object>::New(isolate, perSocketData->socketPf), topicArrayBuffer, Integer::New(isolate, newCount), Integer::New(isolate, oldCount)};
            CallJS(isolate, Local<Function>::New(isolate, subscriptionPf), 4, argv);

            topicArrayBuffer->Detach();
        };
    }

    /* Ping handler is always optional */
    if (pingPf != Undefined(isolate)) {
        behavior.ping = [pingPf = std::move(pingPf), isolate](auto *ws, std::string_view message) {
            HandleScope hs(isolate);

            Local<ArrayBuffer> messageArrayBuffer = ArrayBuffer_New(isolate, (void *) message.data(), message.length());
            PerSocketData *perSocketData = (PerSocketData *) ws->getUserData();
            Local<Value> argv[2] = {Local<Object>::New(isolate, perSocketData->socketPf), messageArrayBuffer};
            CallJS(isolate, Local<Function>::New(isolate, pingPf), 2, argv);

            messageArrayBuffer->Detach();
        };
    }

    /* Pong handler is always optional */
    if (pongPf != Undefined(isolate)) {
        behavior.pong = [pongPf = std::move(pongPf), isolate](auto *ws, std::string_view message) {
            HandleScope hs(isolate);

            Local<ArrayBuffer> messageArrayBuffer = ArrayBuffer_New(isolate, (void *) message.data(), message.length());
            PerSocketData *perSocketData = (PerSocketData *) ws->getUserData();
            Local<Value> argv[2] = {Local<Object>::New(isolate, perSocketData->socketPf), messageArrayBuffer};
            CallJS(isolate, Local<Function>::New(isolate, pongPf), 2, argv);

            messageArrayBuffer->Detach();
        };
    }

    /* Close handler is NOT optional for the wrapper */
    behavior.close = [closePf = std::move(closePf), isolate](auto *ws, int code, std::string_view message) {
        HandleScope hs(isolate);

        PerSocketData *perSocketData = (PerSocketData *) ws->getUserData();
        Local<Object> wsObject = Local<Object>::New(isolate, perSocketData->socketPf);

        /* Invalidate this wsObject */
        setInternalPointer(wsObject, nullptr);

        /* Only call close handler if we have one set */
        Local<Function> closeLf = Local<Function>::New(isolate, closePf);
        if (!closeLf->IsUndefined()) {
            Local<ArrayBuffer> messageArrayBuffer = ArrayBuffer_New(isolate, (void *) message.data(), message.length());
            Local<Value> argv[3] = {wsObject, Integer::New(isolate, code), messageArrayBuffer};
            CallJS(isolate, closeLf, 3, argv);

            /* Again, here we clear the buffer to avoid strange bugs */
            messageArrayBuffer->Detach();
        }

        /* This should technically not be required */
        perSocketData->socketPf.Reset();
    };
}

/* uWS.App.ws('/pattern', behavior) */
template <typename APP>
void uWS_App_ws(const FunctionCallbackInfo<Value> &args) {
    if (insideRouteDispatch) {
        args.GetReturnValue().Set(args.GetIsolate()->ThrowException(v8::Exception::Error(String::NewFromUtf8(args.GetIsolate(), "uWS: routes cannot be registered from within a request handler", NewStringType::kNormal).ToLocalChecked())));
        return;
    }

    /* pattern, behavior */
    if (missingArguments(2, args)) {
        return;
    }

    Isolate *isolate = args.GetIsolate();

    /* Validate values that would otherwise terminate the native engine. */
    if (!requireObject(args, 1)) {
        return;
    }

    if (args.Length() == 2) {
        Local<Object> behaviorObject = Local<Object>::Cast(args[1]);

        Local<Value> idleTimeoutValue;
        if (!behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "idleTimeout", NewStringType::kNormal).ToLocalChecked()).ToLocal(&idleTimeoutValue)) { return; }
        if (!idleTimeoutValue->IsUndefined()) {
            int idleTimeout;
            if (!requireInt32(args, idleTimeoutValue, &idleTimeout)) {
                return;
            }
            if (idleTimeout != 0 && (idleTimeout < 8 || idleTimeout > 960)) {
                args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::Error(String::NewFromUtf8(isolate, "uWS: idleTimeout must be 0 or between 8 and 960 seconds", NewStringType::kNormal).ToLocalChecked())));
                return;
            }
        }

        Local<Value> maxLifetimeValue;
        if (!behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "maxLifetime", NewStringType::kNormal).ToLocalChecked()).ToLocal(&maxLifetimeValue)) { return; }
        if (!maxLifetimeValue->IsUndefined()) {
            int maxLifetime;
            if (!requireInt32(args, maxLifetimeValue, &maxLifetime)) {
                return;
            }
            if (maxLifetime > 240) {
                args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::Error(String::NewFromUtf8(isolate, "uWS: maxLifetime must not be greater than 240 minutes", NewStringType::kNormal).ToLocalChecked())));
                return;
            }
        }
    }

    PerContextData *perContextData = (PerContextData *) Local<External>::Cast(args.Data())->Value();

    APP *app = (APP *) getInternalPointer(args.This());//->GetAlignedPointerFromInternalField(0);
    /* This one is default constructed with defaults */
    typename APP::template WebSocketBehavior<PerSocketData> behavior = {};

    NativeString pattern(args.GetIsolate(), args[0]);
    if (pattern.isInvalid(args)) {
        return;
    }

    UniquePersistent<Function> upgradePf;
    UniquePersistent<Function> openPf;
    UniquePersistent<Function> messagePf;
    UniquePersistent<Function> drainPf;
    UniquePersistent<Function> closePf;
    UniquePersistent<Function> droppedPf;
    UniquePersistent<Function> pingPf;
    UniquePersistent<Function> pongPf;
    UniquePersistent<Function> subscriptionPf;

    /* Get the behavior object */
    if (args.Length() == 2) {
        Local<Object> behaviorObject = Local<Object>::Cast(args[1]);

        /* maxPayloadLength or default */
        Local<Value> maxPayloadLengthValue;
        if (!behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "maxPayloadLength", NewStringType::kNormal).ToLocalChecked()).ToLocal(&maxPayloadLengthValue)) { return; }
        if (!maxPayloadLengthValue->IsUndefined()) {
            if (!requireInt32(args, maxPayloadLengthValue, &behavior.maxPayloadLength)) return;
        }

        /* idleTimeout or default */
        Local<Value> idleTimeoutValue;
        if (!behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "idleTimeout", NewStringType::kNormal).ToLocalChecked()).ToLocal(&idleTimeoutValue)) { return; }
        if (!idleTimeoutValue->IsUndefined()) {
            if (!requireInt32(args, idleTimeoutValue, &behavior.idleTimeout)) return;
        }

        /* maxLifetime or default */
        Local<Value> maxLifetimeValue;
        if (!behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "maxLifetime", NewStringType::kNormal).ToLocalChecked()).ToLocal(&maxLifetimeValue)) { return; }
        if (!maxLifetimeValue->IsUndefined()) {
            /* Cap at 239 to avoid modulo wraparound in uSockets (240 % 240 == 0 == current timestamp, causing immediate timeout), and ensure non-negative */
            int maxLifetime;
            if (!requireInt32(args, maxLifetimeValue, &maxLifetime)) return;
            behavior.maxLifetime = std::max(0, std::min<int>(maxLifetime, 239));
        }

        /* closeOnBackpressureLimit or default */
        Local<Value> closeOnBackpressureLimitValue;
        if (!behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "closeOnBackpressureLimit", NewStringType::kNormal).ToLocalChecked()).ToLocal(&closeOnBackpressureLimitValue)) { return; }
        if (!closeOnBackpressureLimitValue->IsUndefined()) {
            behavior.closeOnBackpressureLimit = closeOnBackpressureLimitValue->BooleanValue(isolate);
        }

        /* sendPingsAutomatically or default */
        Local<Value> sendPingsAutomaticallyValue;
        if (!behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "sendPingsAutomatically", NewStringType::kNormal).ToLocalChecked()).ToLocal(&sendPingsAutomaticallyValue)) { return; }
        if (!sendPingsAutomaticallyValue->IsUndefined()) {
            behavior.sendPingsAutomatically = sendPingsAutomaticallyValue->BooleanValue(isolate);
        }

        /* Compression or default, map from 0, 1, 2 to disabled, shared, dedicated. This is actually the enum */
        Local<Value> compressionValue;
        if (!behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "compression", NewStringType::kNormal).ToLocalChecked()).ToLocal(&compressionValue)) { return; }
        if (!compressionValue->IsUndefined()) {
            int compression;
            if (!requireInt32(args, compressionValue, &compression)) return;
            behavior.compression = (uWS::CompressOptions) compression;
        }

        /* maxBackpressure or default */
        Local<Value> maxBackpressureValue;
        if (!behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "maxBackpressure", NewStringType::kNormal).ToLocalChecked()).ToLocal(&maxBackpressureValue)) { return; }
        if (!maxBackpressureValue->IsUndefined()) {
            if (!requireInt32(args, maxBackpressureValue, &behavior.maxBackpressure)) return;
        }

        auto readFunction = [&](const char *name, UniquePersistent<Function> &persistent) {
            Local<Value> value;
            if (!behaviorObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, name, NewStringType::kNormal).ToLocalChecked()).ToLocal(&value)) return false;
            if (!value->IsUndefined() && !requireFunction(args, value)) return false;
            persistent.Reset(isolate, Local<Function>::Cast(value));
            return true;
        };
        if (!readFunction("upgrade", upgradePf) || !readFunction("open", openPf) || !readFunction("message", messagePf) ||
            !readFunction("drain", drainPf) || !readFunction("close", closePf) || !readFunction("dropped", droppedPf) ||
            !readFunction("ping", pingPf) || !readFunction("pong", pongPf) || !readFunction("subscription", subscriptionPf)) return;

    }

    /* Upgrade handler is always optional */
    if (upgradePf != Undefined(isolate)) {
        behavior.upgrade = [upgradePf = std::move(upgradePf), perContextData](auto *res, auto *req, auto *context) {
            Isolate *isolate = perContextData->isolate;
            HandleScope hs(isolate);

            Local<Function> upgradeLf = Local<Function>::New(isolate, upgradePf);
            Local<Object> resObject = perContextData->resTemplate[getAppTypeIndex<APP>()].Get(isolate)->Clone();
            setInternalPointer(resObject, res);

            Local<Object> reqObject = perContextData->reqTemplate.Get(isolate)->Clone();
            setInternalPointer(reqObject, req);

            Local<Value> argv[3] = {resObject, reqObject, External::New(isolate, (void *) context)};
            CallJS(isolate, upgradeLf, 3, argv);

            /* Properly invalidate req */
            setInternalPointer(reqObject, nullptr);

            if (getInternalPointer(resObject)) {
                if (us_socket_is_closed(getAppTypeIndex<APP>(), (struct us_socket_t *) res)) {
                    setInternalPointer(resObject, nullptr);
                } else if (!res->hasResponded() && !res->hasOnAborted()) {
                    UniquePersistent<Object> persistentResObject(isolate, resObject);

                    res->onAborted([resObject = std::move(persistentResObject), isolate]() {
                        HandleScope hs(isolate);

                        setInternalPointer(Local<Object>::New(isolate, resObject), nullptr);
                    });
                    res->armIdleTimeout();
                }
            }
        };
    }

    bindWebSocketBehavior(isolate, perContextData, &perContextData->wsTemplate[getAppTypeIndex<APP>()], behavior,
        std::move(openPf), std::move(messagePf), std::move(drainPf), std::move(closePf),
        std::move(droppedPf), std::move(pingPf), std::move(pongPf), std::move(subscriptionPf));

    app->template ws<PerSocketData>(std::string(pattern.getString()), std::move(behavior));

    /* Return this */
    args.GetReturnValue().Set(args.This());
}

/* This method wraps get, post and all http methods */
template <typename APP, typename F>
void uWS_App_get(F f, const FunctionCallbackInfo<Value> &args) {
    if (insideRouteDispatch) {
        args.GetReturnValue().Set(args.GetIsolate()->ThrowException(v8::Exception::Error(String::NewFromUtf8(args.GetIsolate(), "uWS: routes cannot be registered from within a request handler", NewStringType::kNormal).ToLocalChecked())));
        return;
    }

    APP *app = (APP *) getInternalPointer(args.This());//->GetAlignedPointerFromInternalField(0);

    /* Pattern */
    NativeString pattern(args.GetIsolate(), args[0]);
    if (pattern.isInvalid(args)) {
        return;
    }

    /* If the handler is null */
    if (args[1]->IsNull()) {
        (app->*f)(std::string(pattern.getString()), nullptr);
        args.GetReturnValue().Set(args.This());
        return;
    }

    /* If the handler is String */
    if (args[1]->IsArrayBuffer()) {
        NativeString constantString(args.GetIsolate(), args[1]);
        if (constantString.isInvalid(args)) {
            return;
        }

        (app->*f)(std::string(pattern.getString()), [response = std::string(constantString.getString().data(), constantString.getString().length())](auto *res, auto *req) {
            /* Parse the DeclarativeResponse */
            std::string_view remainingInstructions(response.data(), response.length());
            bool validInstructions = true;
            while (remainingInstructions.length() && validInstructions) {
                switch(remainingInstructions[0]) {
                    case 0: {
                        /* opCode END */
                        if (remainingInstructions.length() < 3) {
                            validInstructions = false;
                            break;
                        }
                        uint16_t length;
                        memcpy(&length, remainingInstructions.data() + 1, 2);
                        remainingInstructions.remove_prefix(3); // Skip opCode and length bytes

                        if (remainingInstructions.length() < length) {
                            validInstructions = false;
                            break;
                        }
                        res->end(remainingInstructions.substr(0, length));
                        remainingInstructions.remove_prefix(length);
                    }
                    break;
                    case 1: {
                        /* opCode WRITE_HEADER */
                        if (remainingInstructions.length() < 2) {
                            validInstructions = false;
                            break;
                        }
                        uint8_t keyLength;
                        memcpy(&keyLength, remainingInstructions.data() + 1, 1);
                        remainingInstructions.remove_prefix(2); // Skip opCode and key length bytes

                        if (remainingInstructions.length() < keyLength) {
                            validInstructions = false;
                            break;
                        }
                        std::string_view keyString(remainingInstructions.data(), keyLength);
                        remainingInstructions.remove_prefix(keyLength);

                        if (remainingInstructions.length() < 1) {
                            validInstructions = false;
                            break;
                        }
                        uint8_t valueLength;
                        memcpy(&valueLength, remainingInstructions.data(), 1);
                        remainingInstructions.remove_prefix(1); // Skip value length bytes

                        if (remainingInstructions.length() < valueLength) {
                            validInstructions = false;
                            break;
                        }
                        std::string_view valueString(remainingInstructions.data(), valueLength);
                        remainingInstructions.remove_prefix(valueLength);

                        res->writeHeader(keyString, valueString);
                    }
                    break;
                    case 2: {
                        /* opCode WRITE_BODY */
                        if (remainingInstructions.length() < 1) {
                            validInstructions = false;
                            break;
                        }
                        remainingInstructions.remove_prefix(1); // Skip opCode
                        //res->writeBody();
                    }
                    break;
                    case 3: {
                        /* opCode WRITE_QUERY_VALUE */
                        if (remainingInstructions.length() < 2) {
                            validInstructions = false;
                            break;
                        }
                        uint8_t keyLength;
                        memcpy(&keyLength, remainingInstructions.data() + 1, 1);
                        remainingInstructions.remove_prefix(2); // Skip opCode and key length bytes

                        if (remainingInstructions.length() < keyLength) {
                            validInstructions = false;
                            break;
                        }
                        std::string_view keyString(remainingInstructions.data(), keyLength);
                        remainingInstructions.remove_prefix(keyLength);

                        res->write(req->getQuery(keyString));
                    }
                    break;
                    case 4: {
                        /* opCode WRITE_HEADER_VALUE */
                        if (remainingInstructions.length() < 2) {
                            validInstructions = false;
                            break;
                        }
                        uint8_t keyLength;
                        memcpy(&keyLength, remainingInstructions.data() + 1, 1);
                        remainingInstructions.remove_prefix(2); // Skip opCode and key length bytes

                        if (remainingInstructions.length() < keyLength) {
                            validInstructions = false;
                            break;
                        }
                        std::string_view keyString(remainingInstructions.data(), keyLength);
                        remainingInstructions.remove_prefix(keyLength);

                        res->write(req->getHeader(keyString));
                    }
                    break;
                    case 5: {
                        /* opCode WRITE */
                        if (remainingInstructions.length() < 3) {
                            validInstructions = false;
                            break;
                        }
                        uint16_t length;
                        memcpy(&length, remainingInstructions.data() + 1, 2);
                        remainingInstructions.remove_prefix(3); // Skip opCode and length bytes

                        if (remainingInstructions.length() < length) {
                            validInstructions = false;
                            break;
                        }
                        std::string_view valueString(remainingInstructions.data(), length);
                        remainingInstructions.remove_prefix(length);

                        res->write(valueString);
                    }
                    break;
                    case 6: {
                        /* opCode WRITE_PARAMETER_VALUE */
                        if (remainingInstructions.length() < 2) {
                            validInstructions = false;
                            break;
                        }
                        uint8_t keyLength;
                        memcpy(&keyLength, remainingInstructions.data() + 1, 1);
                        remainingInstructions.remove_prefix(2); // Skip opCode and key length bytes

                        if (remainingInstructions.length() < keyLength) {
                            validInstructions = false;
                            break;
                        }
                        std::string_view keyString(remainingInstructions.data(), keyLength);
                        remainingInstructions.remove_prefix(keyLength);

                        res->write(req->getParameter(keyString));
                    }
                    break;
                    case 7: {
                        /* opCode WRITE_STATUS */
                        if (remainingInstructions.length() < 2) {
                            validInstructions = false;
                            break;
                        }
                        uint8_t statusLength;
                        memcpy(&statusLength, remainingInstructions.data() + 1, 1);
                        remainingInstructions.remove_prefix(2); // Skip opCode and status length bytes

                        if (remainingInstructions.length() < statusLength) {
                            validInstructions = false;
                            break;
                        }
                        std::string_view statusString(remainingInstructions.data(), statusLength);
                        remainingInstructions.remove_prefix(statusLength);

                        res->writeStatus(statusString);
                    }
                    break;
                    default: {
                        /* Unknown opcode: stop rather than spin on a malformed stream */
                        validInstructions = false;
                    }
                    break;
                }
            }

        });

        args.GetReturnValue().Set(args.This());
        return;
    }

    /* Handler */
    Callback checkedCallback(args.GetIsolate(), args[1]);
    if (checkedCallback.isInvalid(args)) {
        return;
    }
    UniquePersistent<Function> cb = checkedCallback.getFunction();

    /* This function requires perContextData */
    PerContextData *perContextData = (PerContextData *) Local<External>::Cast(args.Data())->Value();

    (app->*f)(std::string(pattern.getString()), [cb = std::move(cb), perContextData](auto *res, auto *req) {
        Isolate *isolate = perContextData->isolate;
        HandleScope hs(isolate);

        Local<Object> resObject = perContextData->resTemplate[getAppTypeIndex<APP>()].Get(isolate)->Clone();
        setInternalPointer(resObject, res);

        Local<Object> reqObject = perContextData->reqTemplate.Get(isolate)->Clone();
        setInternalPointer(reqObject, req);

        Local<Value> argv[] = {resObject, reqObject};
        CallJS(isolate, cb.Get(isolate), 2, argv);

        /* Properly invalidate req */
        setInternalPointer(reqObject, nullptr);

        if (getInternalPointer(resObject)) {
            if (us_socket_is_closed(getAppTypeIndex<APP>(), (struct us_socket_t *) res)) {
                setInternalPointer(resObject, nullptr);
            } else if (!res->hasResponded() && !res->hasOnAborted()) {
                UniquePersistent<Object> persistentResObject(isolate, resObject);

                res->onAborted([resObject = std::move(persistentResObject), isolate]() {
                    HandleScope hs(isolate);

                    setInternalPointer(Local<Object>::New(isolate, resObject), nullptr);
                });
                res->armIdleTimeout();
            }
        }
    });

    args.GetReturnValue().Set(args.This());
}

template <typename APP>
void uWS_App_close(const FunctionCallbackInfo<Value> &args) {
    APP *app = (APP *) getInternalPointer(args.This());//->GetAlignedPointerFromInternalField(0);

    app->close();
    args.GetReturnValue().Set(args.This());
}

template <typename APP>
void uWS_App_listen_unix(const FunctionCallbackInfo<Value> &args) {
    APP *app = (APP *) getInternalPointer(args.This());//->GetAlignedPointerFromInternalField(0);

    Isolate *isolate = args.GetIsolate();
    Local<Context> context = isolate->GetCurrentContext();
    Local<FunctionTemplate> listenSocketTemplate = FunctionTemplate::New(isolate);
    listenSocketTemplate->SetClassName(String::NewFromUtf8(isolate, "uWS.ListenSocket", NewStringType::kNormal).ToLocalChecked());
    listenSocketTemplate->InstanceTemplate()->SetInternalFieldCount(1);
    Local<Private> listenSocketMarker = Private::ForApi(isolate, String::NewFromUtf8(isolate, "uWS.ListenSocket", NewStringType::kNormal).ToLocalChecked());
    auto listenSockets = std::make_shared<std::vector<UniquePersistent<Object>>>();
    app->listenSocketInvalidators.emplace_back([listenSockets, isolate] {
        HandleScope hs(isolate);
        for (auto &listenSocket : *listenSockets) {
            setInternalPointer(Local<Object>::New(isolate, listenSocket), nullptr);
        }
        listenSockets->clear();
    });

    /* Require at least two arguments */
    if (missingArguments(2, args)) {
        return;
    }
    if (!requireFunction(args, 0)) {
        return;
    }

    /* integer options is first (not implemented) */

    /* Callback is first */
    auto cb = [&args, isolate, context, listenSocketTemplate, listenSocketMarker, listenSockets](auto *token) {
        /* Return a false boolean if listen failed */
        Local<Value> value = Boolean::New(isolate, false);
        if (token) {
            Local<Object> listenSocket = listenSocketTemplate->GetFunction(context).ToLocalChecked()->NewInstance(context).ToLocalChecked();
            setInternalPointer(listenSocket, token);
            listenSocket->SetPrivate(context, listenSocketMarker, True(isolate)).ToChecked();
            listenSockets->emplace_back(isolate, listenSocket);
            value = listenSocket;
        }
        Local<Value> argv[] = {value};
        /* Immediate call cannot be CallJS */
        Local<Function>::Cast(args[0])->Call(isolate->GetCurrentContext(), isolate->GetCurrentContext()->Global(), 1, argv).IsEmpty();
    };

    /* Path is last */
    std::string path;
    NativeString h(isolate, args[args.Length() - 1]);
    if (h.isInvalid(args)) {
        return;
    }
    path = h.getString();

    app->listen(std::move(cb), path);

    args.GetReturnValue().Set(args.This());
}

template <typename APP>
void uWS_App_listen(const FunctionCallbackInfo<Value> &args) {
    APP *app = (APP *) getInternalPointer(args.This());//->GetAlignedPointerFromInternalField(0);

    Isolate *isolate = args.GetIsolate();
    Local<Context> context = isolate->GetCurrentContext();
    Local<FunctionTemplate> listenSocketTemplate = FunctionTemplate::New(isolate);
    listenSocketTemplate->SetClassName(String::NewFromUtf8(isolate, "uWS.ListenSocket", NewStringType::kNormal).ToLocalChecked());
    listenSocketTemplate->InstanceTemplate()->SetInternalFieldCount(1);
    Local<Private> listenSocketMarker = Private::ForApi(isolate, String::NewFromUtf8(isolate, "uWS.ListenSocket", NewStringType::kNormal).ToLocalChecked());
    auto listenSockets = std::make_shared<std::vector<UniquePersistent<Object>>>();
    app->listenSocketInvalidators.emplace_back([listenSockets, isolate] {
        HandleScope hs(isolate);
        for (auto &listenSocket : *listenSockets) {
            setInternalPointer(Local<Object>::New(isolate, listenSocket), nullptr);
        }
        listenSockets->clear();
    });

    /* Require at least two arguments */
    if (missingArguments(2, args)) {
        return;
    }
    if (!requireFunction(args, args.Length() - 1)) {
        return;
    }

    /* Callback is last */
    auto cb = [&args, isolate, context, listenSocketTemplate, listenSocketMarker, listenSockets](auto *token) {
        /* Return a false boolean if listen failed */
        Local<Value> value = Boolean::New(isolate, false);
        if (token) {
            Local<Object> listenSocket = listenSocketTemplate->GetFunction(context).ToLocalChecked()->NewInstance(context).ToLocalChecked();
            setInternalPointer(listenSocket, token);
            listenSocket->SetPrivate(context, listenSocketMarker, True(isolate)).ToChecked();
            listenSockets->emplace_back(isolate, listenSocket);
            value = listenSocket;
        }
        Local<Value> argv[] = {value};
        /* Immediate call cannot be CallJS */
        Local<Function>::Cast(args[args.Length() - 1])->Call(isolate->GetCurrentContext(), isolate->GetCurrentContext()->Global(), 1, argv).IsEmpty();
    };

    /* Host is first, if present */
    std::string host;
    if (!args[0]->IsNumber()) {
        NativeString h(isolate, args[0]);
        if (h.isInvalid(args)) {
            return;
        }
        host = h.getString();
    }

    /* Port, options are in the middle, if present */
    std::vector<int> numbers;
    for (int i = (args[0]->IsNumber() ? 0 : 1); i < args.Length() - 1; i++) {
        if (!args[i]->IsNumber()) {
            args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::Error(String::NewFromUtf8(isolate, "Expected a number.", NewStringType::kNormal).ToLocalChecked())));
            return;
        }
        numbers.push_back(args[i]->Uint32Value(args.GetIsolate()->GetCurrentContext()).FromMaybe(0));
    }

    /* We only use the most complete overload */
    app->listen(host, numbers.size() ? numbers[0] : 0,
                numbers.size() > 1 ? numbers[1] : 0, std::move(cb));

    args.GetReturnValue().Set(args.This());
}

template <typename APP>
void uWS_App_filter(const FunctionCallbackInfo<Value> &args) {
    APP *app = (APP *) getInternalPointer(args.This());//->GetAlignedPointerFromInternalField(0);

    /* Handler */
    Callback checkedCallback(args.GetIsolate(), args[0]);
    if (checkedCallback.isInvalid(args)) {
        return;
    }
    UniquePersistent<Function> cb = checkedCallback.getFunction();

    /* This function requires perContextData */
    PerContextData *perContextData = (PerContextData *) Local<External>::Cast(args.Data())->Value();

    app->filter([cb = std::move(cb), perContextData](auto *res, int count) {
        Isolate *isolate = perContextData->isolate;
        HandleScope hs(isolate);

        Local<Object> resObject = perContextData->resTemplate[getAppTypeIndex<APP>()].Get(isolate)->Clone();
        setInternalPointer(resObject, res);

        Local<Value> argv[] = {resObject, Local<Value>::Cast(Integer::New(isolate, count))};
        CallJS(isolate, cb.Get(isolate), 2, argv);
        setInternalPointer(resObject, nullptr);
    });

    args.GetReturnValue().Set(args.This());
}

template <typename APP>
void uWS_App_domain(const FunctionCallbackInfo<Value> &args) {
    if (insideRouteDispatch) {
        args.GetReturnValue().Set(args.GetIsolate()->ThrowException(v8::Exception::Error(String::NewFromUtf8(args.GetIsolate(), "uWS: routes cannot be registered from within a request handler", NewStringType::kNormal).ToLocalChecked())));
        return;
    }

    APP *app = (APP *) getInternalPointer(args.This());//->GetAlignedPointerFromInternalField(0);

    Isolate *isolate = args.GetIsolate();

    /* serverName */
    if (missingArguments(1, args)) {
        return;
    }

    NativeString serverName(isolate, args[0]);
    if (serverName.isInvalid(args)) {
        return;
    }

    app->domain(std::string(serverName.getString()));

    args.GetReturnValue().Set(args.This());
}

template <typename APP>
void uWS_App_publish(const FunctionCallbackInfo<Value> &args) {
    APP *app = (APP *) getInternalPointer(args.This());//->GetAlignedPointerFromInternalField(0);

    Isolate *isolate = args.GetIsolate();

    /* topic, message [isBinary, compress] */
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

    bool ok = app->publish(topic.getString(), message.getString(), args[2]->BooleanValue(isolate) ? uWS::OpCode::BINARY : uWS::OpCode::TEXT, args[3]->BooleanValue(isolate));

    args.GetReturnValue().Set(Boolean::New(isolate, ok));
}

template <typename APP>
void uWS_App_numSubscribers(const FunctionCallbackInfo<Value> &args) {
    APP *app = (APP *) getInternalPointer(args.This());//->GetAlignedPointerFromInternalField(0);

    Isolate *isolate = args.GetIsolate();

    /* topic */
    if (missingArguments(1, args)) {
        return;
    }

    NativeString topic(isolate, args[0]);
    if (topic.isInvalid(args)) {
        return;
    }

    args.GetReturnValue().Set(Integer::New(isolate, app->numSubscribers(topic.getString())));
}

/* This one modified per-thread static strings temporarily */
std::pair<uWS::SocketContextOptions, bool> readOptionsObject(const FunctionCallbackInfo<Value> &args, int index) {
    Isolate *isolate = args.GetIsolate();
    /* Read the options object if any */
    uWS::SocketContextOptions options = {};
    thread_local std::string keyFileName, certFileName, passphrase, dhParamsFileName, caFileName, sslCiphers, caPem;
    /* Client contexts verify by default (SSL_VERIFY_PEER). Only consulted by openssl.c when client_mode
     * is set (by the Client/SSLClient wrapper), so this is inert for server App()/SSLApp(). */
    options.reject_unauthorized = 1;
    if (args.Length() > index && !args[index]->IsUndefined() && !args[index]->IsNull()) {

        if (!args[index]->IsObject()) {
            args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::Error(String::NewFromUtf8(isolate, "Options must be an object.", NewStringType::kNormal).ToLocalChecked())));
            return {};
        }

        Local<Object> optionsObject = Local<Object>::Cast(args[index]);

        /* Key file name */
        Local<Value> keyFileNameValueProperty;
        if (!optionsObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "key_file_name", NewStringType::kNormal).ToLocalChecked()).ToLocal(&keyFileNameValueProperty)) { return {}; }
        NativeString keyFileNameValue(isolate, keyFileNameValueProperty);
        if (keyFileNameValue.isInvalid(args)) {
            return {};
        }
        if (keyFileNameValue.getString().length()) {
            keyFileName = keyFileNameValue.getString();
            options.key_file_name = keyFileName.c_str();
        }

        /* Cert file name */
        Local<Value> certFileNameValueProperty;
        if (!optionsObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "cert_file_name", NewStringType::kNormal).ToLocalChecked()).ToLocal(&certFileNameValueProperty)) { return {}; }
        NativeString certFileNameValue(isolate, certFileNameValueProperty);
        if (certFileNameValue.isInvalid(args)) {
            return {};
        }
        if (certFileNameValue.getString().length()) {
            certFileName = certFileNameValue.getString();
            options.cert_file_name = certFileName.c_str();
        }

        /* Passphrase */
        Local<Value> passphraseValueProperty;
        if (!optionsObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "passphrase", NewStringType::kNormal).ToLocalChecked()).ToLocal(&passphraseValueProperty)) { return {}; }
        NativeString passphraseValue(isolate, passphraseValueProperty);
        if (passphraseValue.isInvalid(args)) {
            return {};
        }
        if (passphraseValue.getString().length()) {
            passphrase = passphraseValue.getString();
            options.passphrase = passphrase.c_str();
        }

        /* DH params file name */
        Local<Value> dhParamsFileNameValueProperty;
        if (!optionsObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "dh_params_file_name", NewStringType::kNormal).ToLocalChecked()).ToLocal(&dhParamsFileNameValueProperty)) { return {}; }
        NativeString dhParamsFileNameValue(isolate, dhParamsFileNameValueProperty);
        if (dhParamsFileNameValue.isInvalid(args)) {
            return {};
        }
        if (dhParamsFileNameValue.getString().length()) {
            dhParamsFileName = dhParamsFileNameValue.getString();
            options.dh_params_file_name = dhParamsFileName.c_str();
        }

        /* CA file name */
        Local<Value> caFileNameValueProperty;
        if (!optionsObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "ca_file_name", NewStringType::kNormal).ToLocalChecked()).ToLocal(&caFileNameValueProperty)) { return {}; }
        NativeString caFileNameValue(isolate, caFileNameValueProperty);
        if (caFileNameValue.isInvalid(args)) {
            return {};
        }
        if (caFileNameValue.getString().length()) {
            caFileName = caFileNameValue.getString();
            options.ca_file_name = caFileName.c_str();
        }

        /* ssl_prefer_low_memory_usage */
        Local<Value> sslPreferLowMemoryUsageValue;
        if (!optionsObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "ssl_prefer_low_memory_usage", NewStringType::kNormal).ToLocalChecked()).ToLocal(&sslPreferLowMemoryUsageValue)) { return {}; }
        options.ssl_prefer_low_memory_usage = sslPreferLowMemoryUsageValue->BooleanValue(isolate);

        /* ssl_ciphers */
        Local<Value> sslCiphersValueProperty;
        if (!optionsObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "ssl_ciphers", NewStringType::kNormal).ToLocalChecked()).ToLocal(&sslCiphersValueProperty)) { return {}; }
        NativeString sslCiphersValue(isolate, sslCiphersValueProperty);
        if (sslCiphersValue.isInvalid(args)) {
            return {};
        }
        if (sslCiphersValue.getString().length()) {
            sslCiphers = sslCiphersValue.getString();
            options.ssl_ciphers = sslCiphers.c_str();
        }

        /* ca_pem: in-memory PEM CA bundle (client contexts). Alternative/addition to ca_file_name. */
        Local<Value> caPemValueProperty;
        if (!optionsObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "ca_pem", NewStringType::kNormal).ToLocalChecked()).ToLocal(&caPemValueProperty)) { return {}; }
        NativeString caPemValue(isolate, caPemValueProperty);
        if (caPemValue.isInvalid(args)) {
            return {};
        }
        if (caPemValue.getString().length()) {
            caPem = caPemValue.getString();
            options.ca_pem = caPem.c_str();
        }

        /* reject_unauthorized: client-only cert/hostname verification toggle (default true above). Only
         * consulted by openssl.c under client_mode, so harmless for server contexts. */
        Local<Value> rejectUnauthorizedValue;
        if (!optionsObject->Get(isolate->GetCurrentContext(), String::NewFromUtf8(isolate, "reject_unauthorized", NewStringType::kNormal).ToLocalChecked()).ToLocal(&rejectUnauthorizedValue)) { return {}; }
        if (!rejectUnauthorizedValue->IsUndefined()) {
            options.reject_unauthorized = rejectUnauthorizedValue->BooleanValue(isolate) ? 1 : 0;
        }
    }

    return {options, true};
}

template <typename APP>
void uWS_App_addServerName(const FunctionCallbackInfo<Value> &args) {
    APP *app = (APP *) getInternalPointer(args.This());//->GetAlignedPointerFromInternalField(0);

    Isolate *isolate = args.GetIsolate();
    NativeString hostnamePatternValue(isolate, args[0]);
    if (hostnamePatternValue.isInvalid(args)) {
        return;
    }
    std::string hostnamePattern;
    if (hostnamePatternValue.getString().length()) {
        hostnamePattern = hostnamePatternValue.getString();
    }

    auto [options, valid] = readOptionsObject(args, 1);
    if (!valid) {
        return;
    }

    if (!app->addServerName(hostnamePattern.c_str(), options)) {
        isolate->ThrowException(Exception::Error(String::NewFromUtf8(isolate, "App: addServerName failed", NewStringType::kNormal).ToLocalChecked()));
        return;
    }

    args.GetReturnValue().Set(args.This());
}

template <typename APP>
void uWS_App_removeServerName(const FunctionCallbackInfo<Value> &args) {
    APP *app = (APP *) getInternalPointer(args.This());//->GetAlignedPointerFromInternalField(0);

    Isolate *isolate = args.GetIsolate();
    NativeString hostnamePatternValue(isolate, args[0]);
    if (hostnamePatternValue.isInvalid(args)) {
        return;
    }
    std::string hostnamePattern;
    if (hostnamePatternValue.getString().length()) {
        hostnamePattern = hostnamePatternValue.getString();
    }

    app->removeServerName(hostnamePattern.c_str());

    args.GetReturnValue().Set(args.This());
}

template <typename APP>
void uWS_App_missingServerName(const FunctionCallbackInfo<Value> &args) {
    APP *app = (APP *) getInternalPointer(args.This());//->GetAlignedPointerFromInternalField(0);
    Isolate *isolate = args.GetIsolate();

    if (!requireFunction(args, 0)) {
        return;
    }

    UniquePersistent<Function> missingPf;
    missingPf.Reset(args.GetIsolate(), Local<Function>::Cast(args[0]));

    app->missingServerName([missingPf = std::move(missingPf), isolate](const char *hostname) {
        /* We hand a JavaScript string here */
        HandleScope hs(isolate);
        Local<Function> missingLf = Local<Function>::New(isolate, missingPf);
        Local<Value> argv[1] = {String::NewFromUtf8(isolate, hostname, NewStringType::kNormal).ToLocalChecked()};
        CallJS(isolate, missingLf, 1, argv);
    });

    args.GetReturnValue().Set(args.This());
}

template <typename APP>
void uWS_App(const FunctionCallbackInfo<Value> &args) {

    Isolate *isolate = args.GetIsolate();

    auto [options, valid] = readOptionsObject(args, 0);
    if (!valid) {
        return;
    }

    /* uSockets copies strings here */
    APP *app = new APP(options);

    /* Throw if we failed to construct the app */
    if (app->constructorFailed()) {
        delete app;
        args.GetReturnValue().Set(isolate->ThrowException(v8::Exception::Error(String::NewFromUtf8(isolate, "App construction failed", NewStringType::kNormal).ToLocalChecked())));
        return;
    }

    PerContextData *perContextData = (PerContextData *) Local<External>::Cast(args.Data())->Value();
    constexpr int appTypeIndex = getAppTypeIndex<APP>();

    /* Build the App function template once per context; subsequent App() calls only NewInstance it. */
    if (perContextData->appTemplate[appTypeIndex].IsEmpty()) {
        Local<FunctionTemplate> appTemplate = FunctionTemplate::New(isolate);
        appTemplate->SetClassName(String::NewFromUtf8(isolate, std::is_same<APP, uWS::SSLApp>::value ? "uWS.SSLApp" : "uWS.App", NewStringType::kNormal).ToLocalChecked());

        appTemplate->InstanceTemplate()->SetInternalFieldCount(1);

    /* All the http methods */
    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "get", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, [](auto &args) {

        if constexpr (std::is_same<APP, uWS::App>::value) {
            uWS_App_get<APP>(&uWS::TemplatedApp<false>::get, args);
        } else if constexpr (std::is_same<APP, uWS::SSLApp>::value) {
            uWS_App_get<APP>(&uWS::TemplatedApp<true>::get, args);
        }

    }, args.Data()));

    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "post", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, [](auto &args) {
        uWS_App_get<APP>(&APP::post, args);
    }, args.Data()));

    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "options", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, [](auto &args) {
        uWS_App_get<APP>(&APP::options, args);
    }, args.Data()));

    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "del", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, [](auto &args) {
        uWS_App_get<APP>(&APP::del, args);
    }, args.Data()));

    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "patch", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, [](auto &args) {
        uWS_App_get<APP>(&APP::patch, args);
    }, args.Data()));

    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "put", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, [](auto &args) {
        uWS_App_get<APP>(&APP::put, args);
    }, args.Data()));

    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "head", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, [](auto &args) {
        uWS_App_get<APP>(&APP::head, args);
    }, args.Data()));

    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "connect", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, [](auto &args) {
        uWS_App_get<APP>(&APP::connect, args);
    }, args.Data()));

    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "trace", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, [](auto &args) {
        uWS_App_get<APP>(&APP::trace, args);
    }, args.Data()));

    /* Any http method */
    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "any", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, [](auto &args) {
        uWS_App_get<APP>(&APP::any, args);
    }, args.Data()));

    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "listen", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App_listen<APP>, args.Data()));

    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "close", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App_close<APP>, args.Data()));
    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "listen_unix", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App_listen_unix<APP>, args.Data()));
    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "filter", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App_filter<APP>, args.Data()));

    /* ws, listen */
    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "ws", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App_ws<APP>, args.Data()));
    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "publish", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App_publish<APP>, args.Data()));
    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "numSubscribers", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App_numSubscribers<APP>, args.Data()));

    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "domain", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App_domain<APP>, args.Data()));

    /* SNI */
    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "addServerName", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App_addServerName<APP>, args.Data()));
    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "removeServerName", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App_removeServerName<APP>, args.Data()));
    appTemplate->PrototypeTemplate()->Set(String::NewFromUtf8(isolate, "missingServerName", NewStringType::kNormal).ToLocalChecked(), FunctionTemplate::New(isolate, uWS_App_missingServerName<APP>, args.Data()));

        perContextData->appTemplate[appTypeIndex].Reset(isolate, appTemplate);
    }

    Local<FunctionTemplate> appTemplate = perContextData->appTemplate[appTypeIndex].Get(isolate);

    Local<Object> localApp = appTemplate->GetFunction(isolate->GetCurrentContext()).ToLocalChecked()->NewInstance(isolate->GetCurrentContext()).ToLocalChecked();
    setInternalPointer(localApp, app);

    /* Add this to our delete list */
    if constexpr (std::is_same<APP, uWS::SSLApp>::value) {
        perContextData->sslApps.emplace_back(app);
    } else {
        perContextData->apps.emplace_back(app);
    }

    args.GetReturnValue().Set(localApp);

}
