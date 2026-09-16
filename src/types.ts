/** Options used when constructing an app. Especially for SSLApp.
 * These are options passed directly to uSockets, C layer.
 */
interface AppOptions {
    ca_file_name?: RecognizedString;
    cert_file_name?: RecognizedString;
    dh_params_file_name?: RecognizedString;
    key_file_name?: RecognizedString;
    /** Maximum size in bytes of one request's request line plus header block. Requests over the limit
     * are rejected with 431 and the connection is closed. Set once at construction. Defaults to 16384.
     * A separate, fixed cap of 98 headers also yields 431 and is not affected by this option. */
    maxHeaderSize?: number;
    passphrase?: RecognizedString;
    ssl_ciphers?: RecognizedString;
    /** This translates to SSL_MODE_RELEASE_BUFFERS */
    ssl_prefer_low_memory_usage?: boolean;
}

/** Options for Client / SSLClient. TLS fields apply to SSLClient only. */
interface ClientOptions {
    ca_file_name?: RecognizedString;
    /** In-memory PEM CA bundle. Defaults to Node's default CA set, including NODE_EXTRA_CA_CERTS, for SSLClient when neither CA option is given. */
    ca_pem?: RecognizedString;
    cert_file_name?: RecognizedString;
    key_file_name?: RecognizedString;
    passphrase?: RecognizedString;
    /** Default HTTP CONNECT proxy for every connect(): http://[user:pass@]host:port. Overridden per connect by ConnectBehavior.proxy. */
    proxy?: string;
    /** Verify the server certificate and hostname. Defaults to true. */
    reject_unauthorized?: boolean;
    ssl_ciphers?: RecognizedString;
    ssl_prefer_low_memory_usage?: boolean;
}

/** WebSocket compression options. Combine any compressor with any decompressor using bitwise OR. */
type CompressOptions = number;

/** Settings and handlers for one outbound WebSocket. The handler contract is identical to WebSocketBehavior. */
interface ConnectBehavior<UserData> extends Omit<WebSocketBehavior<UserData>, 'upgrade'> {
    /** Milliseconds for DNS + TCP connect to the destination. 0 disables. Defaults to 10000. */
    connectTimeout?: number;
    /** Called at most once, instead of open, when the connection fails before the handshake completes. */
    failed?: (error: ConnectError) => void;
    /** Milliseconds allowed after TCP connect for TLS and the HTTP 101 exchange. Defaults to 10000. */
    handshakeTimeout?: number;
    /** Extra request headers. Host, Upgrade, Connection and Sec-WebSocket-* are reserved and rejected. */
    headers?: Record<string, string>;
    /** Offered Sec-WebSocket-Protocol values in preference order. Values may not contain commas or CR/LF. */
    protocols?: string[];
    /** HTTP CONNECT proxy for this connection: http://[user:pass@]host:port. Overrides ClientOptions.proxy; '' disables it. */
    proxy?: string;
    /** Per-connection override of reject_unauthorized (SSLClient only). */
    rejectUnauthorized?: boolean;
    /** SNI and verification hostname override (SSLClient only). Defaults to the URL host. */
    servername?: string;
    /** Local IP literal to bind before connecting. A non-literal fails the connect. */
    sourceHost?: string;
    /** Object returned by ws.getUserData(), like the userData passed to res.upgrade(). */
    userData?: UserData;
}

/** Why an outbound connection never reached open. */
interface ConnectError {
    /** Delivered to failed: errno name (ECONNREFUSED, ETIMEDOUT, ECONNRESET, ECONNABORTED, EHOSTUNREACH, ENETUNREACH, EADDRNOTAVAIL, EADDRINUSE, EACCES, ENETDOWN, EMFILE, ENFILE, ENOBUFS, ENOMEM, EINVAL, EAFNOSUPPORT, EUNKNOWN), EAI_* for DNS, or HTTP_STATUS, HANDSHAKE_INVALID, TLS_VERIFY, TLS_HANDSHAKE, PROXY_AUTH or PROXY_STATUS. connect() throws URL_INVALID, HEADER_INVALID and CLIENT_CLOSED synchronously; they are never delivered to failed. */
    code: string;
    /** Response headers when code is HTTP_STATUS, PROXY_AUTH or PROXY_STATUS (lower-cased keys). Redirects are not followed: a 3xx is HTTP_STATUS and exposes location for the caller to act on. Includes proxy-authenticate for PROXY_AUTH. */
    headers?: Record<string, string>;
    message: string;
    /** HTTP status when code is HTTP_STATUS, PROXY_AUTH (always 407) or PROXY_STATUS. */
    status?: number;
}

/** An HttpRequest is stack allocated and only accessible during the callback invocation. */
interface HttpRequest {
    /** Loops over all headers. */
    forEach(cb: (key: string, value: string) => void): void;
    /** Returns the HTTP method as-is. */
    getCaseSensitiveMethod(): string;
    /** Returns the lowercased header value or empty string. */
    getHeader(lowerCaseKey: RecognizedString): string;
    /** Returns the lowercased HTTP method, useful for "any" routes. */
    getMethod(): string;
    /** Returns the parsed parameter at index. Corresponds to route. Can also take the name of the parameter. */
    getParameter(index: number | RecognizedString): string;
    /** Returns the raw querystring (the part of URL after ? sign) or undefined when there is none. */
    getQuery(): string | undefined;
    /** Returns a decoded query parameter value or undefined. */
    getQuery(key: string): string | undefined;
    /** Returns the URL including initial /slash */
    getUrl(): string;
    /** Setting yield to true is to say that this route handler did not handle the route, causing the router to continue looking for a matching route handler, or fail. */
    setYield(_yield: boolean): HttpRequest;
}

/** An HttpResponse is valid until either onAborted callback or any of the .end/.tryEnd calls succeed. You may attach user data to this object. */
interface HttpResponse {
    /** Arbitrary user data may be attached to this object */
    [key: string]: any;
    /** Begins chunked encoding mode and flushes headers immediately. */
    beginWrite(): HttpResponse;
    /** Immediately force closes the connection. Any onAborted callback will run. */
    close(): HttpResponse;
    /** collectBody is a helper function making optimal use of the new onDataV2.
     * It allows efficient and easy collection of smallish HTTP request body data into RAM.
     * It accumulates all data chunks and calls handler with the complete body as an ArrayBuffer once all data has arrived.
     * Treat the body buffer as callback-scoped: valid only synchronously within handler and detached (neutered) afterward.
     * To retain it (store it or use it in a timer, promise or await continuation), you MUST copy it synchronously with fullBody.slice(0).
     * If the total body size exceeds maxSize bytes, handler is called with null instead. */
    collectBody(maxSize: number, handler: (fullBody: ArrayBuffer | null) => void): HttpResponse;
    /** Corking a response is a performance improvement in both CPU and network, as you ready the IO system for writing multiple chunks at once.
     * Takes a callback in which you execute the writeHeader, writeStatus and such calls, in one atomic IO operation.
     */
    cork(cb: () => void): HttpResponse;
    /** Ends this response by copying the contents of body. */
    end(body?: RecognizedString, closeConnection?: boolean): HttpResponse;
    /** Ends this response without a body. */
    endWithoutBody(reportedContentLength?: number, closeConnection?: boolean): HttpResponse;
    /** Returns the remote IP address in binary format (4 or 16 bytes). */
    getRemoteAddress(): ArrayBuffer;
    /** Returns the remote IP address as text. */
    getRemoteAddressAsText(): string;
    /** Returns the remote port number. */
    getRemotePort(): number;
    /** Returns the remote IP address in binary format (4 or 16 bytes), as reported by the PROXY Protocol v2 compatible proxy. */
    getProxiedRemoteAddress(): ArrayBuffer;
    /** Returns the remote IP address as text, as reported by the PROXY Protocol v2 compatible proxy. */
    getProxiedRemoteAddressAsText(): string;
    /** Returns the remote port number, as reported by the PROXY Protocol v2 compatible proxy. */
    getProxiedRemotePort(): number;
    /** Returns the global byte write offset for this response. Use with onWritable. */
    getWriteOffset(): number;
    /** Returns the peer certificate in PEM format, or empty string when absent. SSL only. */
    getX509Certificate(): string;
    /** Every HttpResponse MUST have an attached abort handler IF you do not respond
     * to it immediately inside of the callback. */
    onAborted(handler: () => void): HttpResponse;
    /** Handler for reading HTTP request body data.
     * Every chunk, including isLast === true, is callback-scoped: valid only synchronously within handler and detached (neutered) afterward.
     * To retain it (store it or use it in a timer, promise or await continuation), you MUST copy it synchronously with chunk.slice(0). */
    onData(handler: (chunk: ArrayBuffer, isLast: boolean) => void): HttpResponse;
    /** Handler for reading HTTP request body data. V2.
     * Every chunk is callback-scoped: valid only synchronously within handler and detached (neutered) afterward.
     * To retain it (store it or use it in a timer, promise or await continuation), you MUST copy it synchronously with chunk.slice(0).
     * maxRemainingBodyLength is the known maximum of the remaining body length. Can be used to preallocate a receive buffer.
     */
    onDataV2(handler: (chunk: ArrayBuffer, maxRemainingBodyLength: bigint) => void): HttpResponse;
    /** Registers a handler for writable events. Continue failed write attempts in here.
     * You MUST return true for success, false for failure.
     */
    onWritable(handler: (offset: number) => boolean): HttpResponse;
    /** Pause HTTP request body streaming (throttle). */
    pause(): HttpResponse;
    /** Resume HTTP request body streaming (unthrottle). */
    resume(): HttpResponse;
    /** Ends this response, or tries to, by streaming appropriately sized chunks of body. Use in conjunction with onWritable. Returns packed flags: bit 0 = ok (no backpressure), bit 1 = hasResponded (fully sent). */
    tryEnd(fullBodyOrChunk: RecognizedString, totalSize: number): number;
    /** Upgrades a HttpResponse to a WebSocket. See UpgradeAsync, UpgradeSync example files. */
    upgrade<UserData>(userData: UserData, secWebSocketKey: RecognizedString, secWebSocketProtocol: RecognizedString, secWebSocketExtensions: RecognizedString, context: us_socket_context_t): void;
    /** Enters or continues chunked encoding mode. Writes part of the response. End with zero length write. Returns true if no backpressure was added. */
    write(chunk: RecognizedString): boolean;
    /** Writes key and value to HTTP response. Keys must not contain control characters or ':', and values must not contain control characters except HTAB. See writeStatus and corking. */
    writeHeader(key: RecognizedString, value: RecognizedString): HttpResponse;
    /** Writes the HTTP status message such as "200 OK". It must not contain control characters and has to be called first in any response. */
    writeStatus(status: RecognizedString): HttpResponse;
}

/** Listen options. LIBUS_LISTEN_DEFAULT = 0, LIBUS_LISTEN_EXCLUSIVE_PORT = 1. */
type ListenOptions = number;

interface MultipartField {
    data: ArrayBuffer;
    filename?: string;
    name: string;
    type?: string;
}

/** Recognized string types, things C++ can read and understand as strings.
 * "String" does not have to mean "text", it can also be "binary".
 *
 * The ArrayBufferView type includes Node.js Buffer, DataView, and TypedArray (Uint8Array, Uint16Array, ...).
 */
type RecognizedString = string | ArrayBuffer | SharedArrayBuffer | ArrayBufferView;

/** TemplatedApp is either an SSL or non-SSL app. See App for more info, read user manual. */
interface TemplatedApp {
    /** Adds a server name. Throws if the TLS options are invalid or the name already exists. `maxHeaderSize` in options is ignored: SNI domains share the app's HTTP context. */
    addServerName(hostname: string, options: AppOptions): TemplatedApp;
    /** Registers an HTTP handler matching specified URL pattern on any HTTP method. */
    any(pattern: RecognizedString, handler: (res: HttpResponse, req: HttpRequest) => void | Promise<void>): TemplatedApp;
    /** Closes all sockets including listen sockets. This will forcefully terminate all connections. */
    close(): TemplatedApp;
    /** Registers an HTTP CONNECT handler matching specified URL pattern. */
    connect(pattern: RecognizedString, handler: (res: HttpResponse, req: HttpRequest) => void | Promise<void>): TemplatedApp;
    /** Registers an HTTP DELETE handler matching specified URL pattern. */
    del(pattern: RecognizedString, handler: (res: HttpResponse, req: HttpRequest) => void | Promise<void>): TemplatedApp;
    /** Browse to SNI domain. Used together with .get, .post and similar to attach routes under SNI domains. */
    domain(domain: string): TemplatedApp;
    /** Attaches a "filter" function to track HTTP socket connections / disconnections. WebSocket upgrades report a disconnection when they leave the HTTP context. */
    filter(cb: (res: HttpResponse, count: number) => void | Promise<void>): TemplatedApp;
    /** Registers an HTTP GET handler matching specified URL pattern. */
    get(pattern: RecognizedString, handler: (res: HttpResponse, req: HttpRequest) => void | Promise<void>): TemplatedApp;
    /** Registers an HTTP HEAD handler matching specified URL pattern. */
    head(pattern: RecognizedString, handler: (res: HttpResponse, req: HttpRequest) => void | Promise<void>): TemplatedApp;
    /** Listens to hostname & port. Callback hands either false or a listen socket. */
    listen(host: RecognizedString, port: number, cb: (listenSocket: us_listen_socket | false) => void | Promise<void>): TemplatedApp;
    /** Listens to hostname & port and sets Listen Options. Callback hands either false or a listen socket. */
    listen(host: RecognizedString, port: number, options: ListenOptions, cb: (listenSocket: us_listen_socket | false) => void | Promise<void>): TemplatedApp;
    /** Listens to port. Callback hands either false or a listen socket. */
    listen(port: number, cb: (listenSocket: us_listen_socket | false) => void | Promise<void>): TemplatedApp;
    /** Listens to port and sets Listen Options. Callback hands either false or a listen socket. */
    listen(port: number, options: ListenOptions, cb: (listenSocket: us_listen_socket | false) => void | Promise<void>): TemplatedApp;
    /** Listens to unix socket. Callback hands either false or a listen socket. */
    listen_unix(cb: (listenSocket: us_listen_socket | false) => void | Promise<void>, path: RecognizedString): TemplatedApp;
    /** Registers a synchronous callback on missing server names. */
    missingServerName(cb: (hostname: string) => void): TemplatedApp;
    /** Returns number of subscribers for this topic. */
    numSubscribers(topic: RecognizedString): number;
    /** Registers an HTTP OPTIONS handler matching specified URL pattern. */
    options(pattern: RecognizedString, handler: (res: HttpResponse, req: HttpRequest) => void | Promise<void>): TemplatedApp;
    /** Registers an HTTP PATCH handler matching specified URL pattern. */
    patch(pattern: RecognizedString, handler: (res: HttpResponse, req: HttpRequest) => void | Promise<void>): TemplatedApp;
    /** Registers an HTTP POST handler matching specified URL pattern. */
    post(pattern: RecognizedString, handler: (res: HttpResponse, req: HttpRequest) => void | Promise<void>): TemplatedApp;
    /** Publishes a message under an exact topic name, for all WebSockets under this app. Topic matching is exact; MQTT wildcards are not supported. See WebSocket.publish. */
    publish(topic: RecognizedString, message: RecognizedString, isBinary?: boolean, compress?: boolean): boolean;
    /** Registers an HTTP PUT handler matching specified URL pattern. */
    put(pattern: RecognizedString, handler: (res: HttpResponse, req: HttpRequest) => void | Promise<void>): TemplatedApp;
    /** Removes a server name. */
    removeServerName(hostname: string): TemplatedApp;
    /** Registers an HTTP TRACE handler matching specified URL pattern. */
    trace(pattern: RecognizedString, handler: (res: HttpResponse, req: HttpRequest) => void | Promise<void>): TemplatedApp;
    /** Registers a handler matching specified URL pattern where WebSocket upgrade requests are caught. */
    ws<UserData>(pattern: RecognizedString, behavior: WebSocketBehavior<UserData>): TemplatedApp;
}

/** Outbound WebSocket client. Client dials ws://, SSLClient dials wss://. */
interface TemplatedClient {
    /** Cancels pending connects and closes all open sockets synchronously, then frees the client on the next loop tick. A handshaking connect receives failed with ECONNRESET; resolving or connecting connects are cancelled silently; open sockets receive close. The client cannot be reused afterwards: connect(), publish() and numSubscribers() after close() throw CLIENT_CLOSED. Creating a Client/SSLClient per reconnect is supported, though reusing one long-lived client is cheaper. */
    close(): TemplatedClient;
    /** Starts an outbound connection. Throws synchronously on an invalid URL, scheme or headers, or CLIENT_CLOSED after close(); all other failures arrive in behavior.failed. */
    connect<UserData>(url: RecognizedString, behavior: ConnectBehavior<UserData>): TemplatedClient;
    /** Returns number of subscribers for this topic among this client's sockets. Throws CLIENT_CLOSED after close(). */
    numSubscribers(topic: RecognizedString): number;
    /** Publishes to all this client's sockets subscribed to topic. Throws CLIENT_CLOSED after close(). */
    publish(topic: RecognizedString, message: RecognizedString, isBinary?: boolean, compress?: boolean): boolean;
}

/** Opaque native listen socket token, only used for the .listen() callback. */
interface us_listen_socket {}

/** Native type representing a raw uSockets struct us_socket_t.
 * Careful with this one, it is entirely unchecked and native so invalid usage will blow up.
 */
interface us_socket {}

/** Native type representing a raw uSockets struct us_socket_context_t.
 * Used while upgrading a WebSocket manually. */
interface us_socket_context_t {}

/** A WebSocket connection that is valid from open to close event. */
interface WebSocket<UserData> {
    /** Forcefully closes this WebSocket. Immediately calls the close handler. No WebSocket close message is sent. */
    close(): void;
    /** See HttpResponse.cork. Takes a function in which the socket is corked (packing many sends into one single syscall/SSL block) */
    cork(cb: () => void): WebSocket<UserData>;
    /** Gracefully closes this WebSocket. Immediately calls the close handler.
     * A WebSocket close message is sent with code and shortMessage.
     */
    end(code?: number, shortMessage?: RecognizedString): void;
    /** Returns the bytes buffered in backpressure. This is similar to the bufferedAmount property in the browser counterpart. */
    getBufferedAmount(): number;
    /** Returns the remote IP address. Note that the returned IP is binary, not text. */
    getRemoteAddress(): ArrayBuffer;
    /** Returns the remote IP address as text. */
    getRemoteAddressAsText(): string;
    /** Returns the remote port number. */
    getRemotePort(): number;
    /** Returns a list of topics this websocket is subscribed to. */
    getTopics(): string[];
    /** Returns the UserData object. */
    getUserData(): UserData;
    /** Returns whether this websocket is subscribed to topic. */
    isSubscribed(topic: RecognizedString): boolean;
    /** Sends a ping control message. Returns sendStatus similar to WebSocket.send (regarding backpressure). */
    ping(message?: RecognizedString): number;
    /** Publishes a message under an exact topic name. Topic matching is exact; MQTT wildcards are not supported. Backpressure is managed according to maxBackpressure and closeOnBackpressureLimit settings. */
    publish(topic: RecognizedString, message: RecognizedString, isBinary?: boolean, compress?: boolean): boolean;
    /** Sends a message. Returns 1 for success, 2 for dropped due to backpressure limit, and 0 for built up backpressure that will drain over time. */
    send(message: RecognizedString, isBinary?: boolean, compress?: boolean): number;
    /** Sends the first fragment of a fragmented message. Use for sending large messages in chunks. */
    sendFirstFragment(message: RecognizedString, isBinary?: boolean, compress?: boolean): number;
    /** Sends a middle fragment of a fragmented message. */
    sendFragment(message: RecognizedString, compress?: boolean): number;
    /** Sends the last fragment of a fragmented message. */
    sendLastFragment(message: RecognizedString, compress?: boolean): number;
    /** Subscribes to an exact topic name. Topic matching is exact; MQTT wildcards are not supported. */
    subscribe(topic: RecognizedString): boolean;
    /** Unsubscribe from a topic. Returns true on success, if the WebSocket was subscribed. */
    unsubscribe(topic: RecognizedString): boolean;
}

/** A structure holding settings and handlers for a WebSocket URL route handler. */
interface WebSocketBehavior<UserData> {
    /** Whether or not we should automatically close the socket when a message is dropped due to backpressure. Defaults to false. */
    closeOnBackpressureLimit?: boolean;
    /** Handler for close event, no matter if error, timeout or graceful close. You may not use WebSocket after this event. */
    close?: (ws: WebSocket<UserData>, code: number, message: ArrayBuffer) => void;
    /** What permessage-deflate compression to use. Defaults to DISABLED. */
    compression?: CompressOptions;
    /** Handler for when WebSocket backpressure drains. Check ws.getBufferedAmount(). */
    drain?: (ws: WebSocket<UserData>) => void;
    /** Handler for a dropped WebSocket message. Messages can be dropped due to specified backpressure settings.
     * May subscribe, unsubscribe, publish, end or close. A subscribe/unsubscribe of the topic currently being published, and any publish issued while a publish or drain is in progress, take effect when that publish or drain completes, in call order. */
    dropped?: (ws: WebSocket<UserData>, message: ArrayBuffer, isBinary: boolean) => void | Promise<void>;
    /** Maximum number of seconds that may pass without sending or receiving a message. Disable by using 0. Defaults to 120. */
    idleTimeout?: number;
    /** Maximum length of allowed backpressure per socket when publishing or sending messages. Defaults to 64 * 1024. */
    maxBackpressure?: number;
    /** Maximum number of minutes a WebSocket may be connected before being closed by the server. 0 disables the feature. Valid values are 0 and 1-239. */
    maxLifetime?: number;
    /** Maximum length of received message. If a client tries to send you a message larger than this, the connection is immediately closed. Defaults to 16 * 1024. */
    maxPayloadLength?: number;
    /** Handler for a WebSocket message. Messages are given as ArrayBuffer no matter if they are binary or not. */
    message?: (ws: WebSocket<UserData>, message: ArrayBuffer, isBinary: boolean) => void | Promise<void>;
    /** Handler for new WebSocket connection. WebSocket is valid from open to close, no errors. */
    open?: (ws: WebSocket<UserData>) => void | Promise<void>;
    /** Handler for received ping control message. Pong messages are automatically sent as per the standard. */
    ping?: (ws: WebSocket<UserData>, message: ArrayBuffer) => void;
    /** Handler for received pong control message. */
    pong?: (ws: WebSocket<UserData>, message: ArrayBuffer) => void;
    /** Whether or not we should automatically send pings to uphold a stable connection given whatever idleTimeout. */
    sendPingsAutomatically?: boolean;
    /** Handler for subscription changes. */
    subscription?: (ws: WebSocket<UserData>, topic: ArrayBuffer, newCount: number, oldCount: number) => void;
    /** Upgrade handler used to intercept HTTP upgrade requests and potentially upgrade to WebSocket. */
    upgrade?: (res: HttpResponse, req: HttpRequest, context: us_socket_context_t) => void | Promise<void>;
}

/** Shape of the compiled native addon. */
interface NativeModule {
    App(options?: AppOptions): TemplatedApp;
    Client(options?: ClientOptions): TemplatedClient;
    DEDICATED_COMPRESSOR: CompressOptions;
    DEDICATED_COMPRESSOR_3KB: CompressOptions;
    DEDICATED_COMPRESSOR_4KB: CompressOptions;
    DEDICATED_COMPRESSOR_8KB: CompressOptions;
    DEDICATED_COMPRESSOR_16KB: CompressOptions;
    DEDICATED_COMPRESSOR_32KB: CompressOptions;
    DEDICATED_COMPRESSOR_64KB: CompressOptions;
    DEDICATED_COMPRESSOR_128KB: CompressOptions;
    DEDICATED_COMPRESSOR_256KB: CompressOptions;
    DEDICATED_DECOMPRESSOR: CompressOptions;
    DEDICATED_DECOMPRESSOR_512B: CompressOptions;
    DEDICATED_DECOMPRESSOR_1KB: CompressOptions;
    DEDICATED_DECOMPRESSOR_2KB: CompressOptions;
    DEDICATED_DECOMPRESSOR_4KB: CompressOptions;
    DEDICATED_DECOMPRESSOR_8KB: CompressOptions;
    DEDICATED_DECOMPRESSOR_16KB: CompressOptions;
    DEDICATED_DECOMPRESSOR_32KB: CompressOptions;
    DISABLED: CompressOptions;
    getParts(body: RecognizedString, contentType: RecognizedString): MultipartField[] | undefined;
    LIBUS_LISTEN_EXCLUSIVE_PORT: ListenOptions;
    SHARED_COMPRESSOR: CompressOptions;
    SHARED_DECOMPRESSOR: CompressOptions;
    SSLApp(options: AppOptions): TemplatedApp;
    SSLClient(options: ClientOptions): TemplatedClient;
    /** Closes a listen socket. This is idempotent; closing an already closed token is a no-op. */
    us_listen_socket_close(listenSocket: us_listen_socket): void;
    /** Returns a socket's local port. Throws if a listen socket token has been closed, including by app.close(). */
    us_socket_local_port(socket: us_socket | us_listen_socket): number;
}


export type {
    AppOptions,
    ClientOptions,
    CompressOptions,
    ConnectBehavior,
    ConnectError,
    HttpRequest,
    HttpResponse,
    ListenOptions,
    MultipartField,
    NativeModule,
    RecognizedString,
    TemplatedApp,
    TemplatedClient,
    us_listen_socket,
    us_socket,
    us_socket_context_t,
    WebSocket,
    WebSocketBehavior
};
