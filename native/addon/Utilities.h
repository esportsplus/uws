#ifndef ADDON_UTILITIES_H
#define ADDON_UTILITIES_H

#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <v8.h>
using namespace v8;

void *getInternalPointer(const Local<Object> &holder) {
    return holder->GetAlignedPointerFromInternalField(0, 0);
}

void setInternalPointer(const Local<Object> &holder, void *value) {
    holder->SetAlignedPointerInInternalField(0, value, 0);
}

/* Unfortunately we _have_ to depend on Node.js crap */
#include <node.h>

MaybeLocal<Value> CallJS(Isolate *isolate, Local<Function> f, int argc, Local<Value> *argv) {
    extern thread_local int insideCorkCallback;
    /* All calls we do into JS are properly corked, except for res.cork, where we increase the counter explicitly */
    insideCorkCallback++;
    /* Slow path. Batching this into one node::CallbackScope per libuv tick was investigated (bounded
     * measurement: bypassing MakeCallback entirely gains ~+3.5% http / +6% ws / +14% ws-client) but
     * REJECTED: the only correct variant drains microtasks/process.nextTick once per tick instead of
     * per callback, an observable reordering of continuations relative to same-tick events, and needs a
     * heap-allocated CallbackScope with a UB-prone resource lifetime across the Loop pre/post handlers.
     * Not worth a partial gain on an already-fast path. */
    auto ret = node::MakeCallback(isolate, isolate->GetCurrentContext()->Global(), f, argc, argv, {0, 0});
    insideCorkCallback--;
    return ret;
}

Local<v8::ArrayBuffer> ArrayBuffer_New(Isolate *isolate, void *data, size_t length) {
    std::unique_ptr<BackingStore> backingStore = ArrayBuffer::NewBackingStore(data, length, [](void* data, size_t length, void* deleter_data) {}, nullptr);
    return ArrayBuffer::New(isolate, std::shared_ptr<BackingStore>(backingStore.release()));
}

Local<v8::ArrayBuffer> ArrayBuffer_NewCopy(Isolate *isolate, void *data, size_t length) {
    Local<ArrayBuffer> ab = ArrayBuffer::New(isolate, length);
    memcpy(ab->GetBackingStore()->Data(), data, length);
    return ab;
}

struct PerSocketData {
    UniquePersistent<Object> socketPf;
};

/* Forward declaration of the outbound client owner (defined in ClientApp.h, fully included only in
 * addon.cpp and ClientAppWrapper.h). PerContextData holds the client apps in unique_ptr vectors that
 * are only ever cleared where ClientApp.h is complete, so a forward declaration suffices here. */
namespace uWS {
    template <bool SSL> struct TemplatedClientApp;
    using ClientApp = TemplatedClientApp<false>;
    using SSLClientApp = TemplatedClientApp<true>;
}

struct PerContextData {
    Isolate *isolate;
    UniquePersistent<Object> reqTemplate;
    UniquePersistent<Object> resTemplate[2]; // 0 = non-SSL, 1 = SSL
    UniquePersistent<Object> wsTemplate[2];         // server ws objects; 0 = non-SSL, 1 = SSL
    UniquePersistent<Object> clientWsTemplate[2];   // outbound client ws objects; 0 = non-SSL, 1 = SSL
    UniquePersistent<FunctionTemplate> appTemplate[2]; // 0 = App, 1 = SSLApp
    UniquePersistent<FunctionTemplate> clientAppTemplate[2]; // 0 = Client, 1 = SSLClient

    /* We hold all apps until free */
    std::vector<std::unique_ptr<uWS::App>> apps;
    std::vector<std::unique_ptr<uWS::SSLApp>> sslApps;
    /* Client apps must be freed BEFORE the server apps and BEFORE the loop, so pending connect
     * requests (and their DNS/timer handles) are cancelled first. */
    std::vector<std::unique_ptr<uWS::ClientApp>> clients;
    std::vector<std::unique_ptr<uWS::SSLClientApp>> sslClients;
};

template <class APP>
static constexpr int getAppTypeIndex() {
    return std::is_same<APP, uWS::SSLApp>::value;
}

/* Index into the client template/app arrays: 0 = Client (ws), 1 = SSLClient (wss). */
template <class CLIENTAPP>
static constexpr int getClientAppTypeIndex() {
    return std::is_same<CLIENTAPP, uWS::SSLClientApp>::value;
}

static inline bool missingArguments(int length, const FunctionCallbackInfo<Value> &args) {
    if (args.Length() < length) {
        std::string message = "Function requires at least ";
        message += std::to_string(length);
        message += " arguments.";
        args.GetReturnValue().Set(args.GetIsolate()->ThrowException(v8::Exception::Error(String::NewFromUtf8(args.GetIsolate(), message.c_str(), NewStringType::kNormal).ToLocalChecked())));
        return true;
    }
    return false;
}

static inline bool requireFunction(const FunctionCallbackInfo<Value> &args, const Local<Value> &value) {
    if (!value->IsFunction()) {
        args.GetReturnValue().Set(args.GetIsolate()->ThrowException(v8::Exception::Error(String::NewFromUtf8(args.GetIsolate(), "Passed callback is not a valid function.", NewStringType::kNormal).ToLocalChecked())));
        return false;
    }
    return true;
}

static inline bool requireFunction(const FunctionCallbackInfo<Value> &args, int index) {
    return requireFunction(args, args[index]);
}

static inline bool requireObject(const FunctionCallbackInfo<Value> &args, const Local<Value> &value) {
    if (!value->IsObject()) {
        args.GetReturnValue().Set(args.GetIsolate()->ThrowException(v8::Exception::Error(String::NewFromUtf8(args.GetIsolate(), "Options must be an object.", NewStringType::kNormal).ToLocalChecked())));
        return false;
    }
    return true;
}

static inline bool requireObject(const FunctionCallbackInfo<Value> &args, int index) {
    return requireObject(args, args[index]);
}

template <typename Integer>
static inline bool requireInt32(const FunctionCallbackInfo<Value> &args, const Local<Value> &value, Integer *result) {
    if (!value->IsNumber()) {
        args.GetReturnValue().Set(args.GetIsolate()->ThrowException(v8::Exception::Error(String::NewFromUtf8(args.GetIsolate(), "Expected a number.", NewStringType::kNormal).ToLocalChecked())));
        return false;
    }
    double number = value->NumberValue(args.GetIsolate()->GetCurrentContext()).FromMaybe(0);
    if (number != number) {
        args.GetReturnValue().Set(args.GetIsolate()->ThrowException(v8::Exception::Error(String::NewFromUtf8(args.GetIsolate(), "Expected a number.", NewStringType::kNormal).ToLocalChecked())));
        return false;
    }
    *result = (Integer) value->Int32Value(args.GetIsolate()->GetCurrentContext()).FromMaybe(0);
    return true;
}

struct Callback {
    bool invalid = false;
    UniquePersistent<Function> f;
    Callback(Isolate *isolate, const Local<Value> &value) {

        if (!value->IsFunction()) {
            invalid = true;
            return;
        }

        f.Reset(isolate, Local<Function>::Cast(value));
    }

    bool isInvalid(const FunctionCallbackInfo<Value> &args) {
        if (invalid) {
            args.GetReturnValue().Set(args.GetIsolate()->ThrowException(v8::Exception::Error(String::NewFromUtf8(args.GetIsolate(), "Passed callback is not a valid function.", NewStringType::kNormal).ToLocalChecked())));
        }
        return invalid;
    }

    UniquePersistent<Function> &&getFunction() {
        return std::move(f);
    }
};

class NativeString {
    char *data;
    size_t length;
    bool allocated = false;
    bool invalid = false;

    // Static thread-local state shared by all NativeString instances on this thread
    inline static thread_local std::vector<char> pool = std::vector<char>(128 * 1024);
    inline static thread_local size_t pool_offset = 0;
    inline static thread_local int ref_count = 0;

    static char* alloc(size_t size) {
        // Ensure size is a multiple of 8
        size = (size + 7) & ~7;

        // Fallback for allocations larger than the remaining pool space
        if (pool_offset + size > pool.size()) {
            // Mark for external cleanup if using instance-based logic
            // (Note: In a pure static alloc, you'd need a way to track this)
            return (char*)std::malloc(size);
        }

        char* ptr = pool.data() + pool_offset;
        pool_offset += size;
        return ptr;
    }

    // Provided for completeness, though the "pool" doesn't actually free individual slices
    static void free(char* ptr) {
        if (ptr < pool.data() || ptr >= pool.data() + pool.size()) {
            ::free(ptr);
        }
    }

public:
    NativeString(Isolate *isolate, const Local<Value> &value) {
        if (ref_count == 0) {
            pool_offset = 0; // Reset the "stack" when entering the first scope
        }
        ref_count++;

        if (value->IsUndefined()) {
            data = nullptr;
            length = 0;
        } else if (value->IsString()) {
            Local<String> string = Local<String>::Cast(value);

            length = string->Utf8LengthV2(isolate);
            data = alloc(length);
            allocated = true;
            string->WriteUtf8V2(isolate, data, length);
        } else if (value->IsArrayBufferView()) { /* DataView or TypedArray */
            Local<ArrayBufferView> arrayBufferView = Local<ArrayBufferView>::Cast(value);
            auto contents = arrayBufferView->Buffer()->GetBackingStore();
            length = arrayBufferView->ByteLength();
            data = (char *) contents->Data() + arrayBufferView->ByteOffset();
        } else if (value->IsArrayBuffer()) {
            Local<ArrayBuffer> arrayBuffer = Local<ArrayBuffer>::Cast(value);
            auto contents = arrayBuffer->GetBackingStore();
            length = contents->ByteLength();
            data = (char *) contents->Data();
        } else if (value->IsSharedArrayBuffer()) {
            Local<SharedArrayBuffer> arrayBuffer = Local<SharedArrayBuffer>::Cast(value);
            auto contents = arrayBuffer->GetBackingStore();
            length = contents->ByteLength();
            data = (char *) contents->Data();
        } else {
            invalid = true;
        }
    }

    bool isInvalid(const FunctionCallbackInfo<Value> &args) {
        if (invalid) {
            args.GetReturnValue().Set(args.GetIsolate()->ThrowException(v8::Exception::Error(String::NewFromUtf8(args.GetIsolate(), "Text and data can only be passed by String, ArrayBuffer or ArrayBufferView.", NewStringType::kNormal).ToLocalChecked())));
        }
        return invalid;
    }

    std::string_view getString() {
        return {data, length};
    }

    ~NativeString() {
        ref_count--;
        if (allocated) {
            free(data);
        }
    }
};

// Utility function to extract raw certificate data
std::string extractX509PemCertificate(SSL* ssl) {
    std::string pemCertificate;

    if (!ssl) {
        return pemCertificate;
    }

    // Get the peer certificate
    X509* peerCertificate = SSL_get_peer_certificate(ssl);
    if (!peerCertificate) {
        // No peer certificate available
        return pemCertificate;
    }

    // Convert X509 certificate to PEM format
    BIO* bio = BIO_new(BIO_s_mem());
    if(bio) {
        if (PEM_write_bio_X509(bio, peerCertificate)) {
            char* buffer;
            long length = BIO_get_mem_data(bio, &buffer);
            pemCertificate.assign(buffer, length);
        }
        BIO_free(bio);
    }

    // Free the peer certificate
    X509_free(peerCertificate);
    return pemCertificate;
}

#endif
