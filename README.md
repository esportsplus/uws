# @esportsplus/uws

Private fork of [uWebSockets.js](https://github.com/uNetworking/uWebSockets.js) by Alex Hultman / uNetworking AB, with the [uWebSockets](https://github.com/uNetworking/uWebSockets) C++ engine and the [uSockets](https://github.com/uNetworking/uSockets) C event loop flattened into this single repository. Upstream git history is not carried; this tree is maintained independently from the point of the fork.

Fork point:

This fork uses its own version sequence starting at `0.0.1`, with a fresh main
history. Earlier `20.67.x` npm releases remain available; `latest` follows the new
sequence. Upstream versions below identify provenance, not this package's version.

| Component | Upstream commit | Date |
|---|---|---|
| uWebSockets.js v20.67.0 | `faf115275bb9c55edf739a06406849e42e89ec04` | 2026-07-08 |
| uWebSockets | `fe7c01a477b688a7743f754fee33bdd78d52ad91` | 2026-07-07 |
| uSockets | `86097c490263ab662d62e8e7b541390bdec7d149` | 2026-04-13 |

## Layout

```
native/addon/          Node addon (JS <-> C++ boundary)
native/uwebsockets/    HTTP, WebSocket, pub/sub, router (header-only C++)
native/usockets/       event loop, sockets, TLS glue (C)
src/                   TypeScript entry point and API typings
src/native.ts          fetches pinned third-party deps into deps/ and compiles dist/*.node
build/                 emitted JavaScript and declarations (tsc, published)
dist/                  compiled addon, one file per platform, arch and Node ABI
bench/                 autocannon and WebSocket echo harnesses
test/                  Vitest suite, closure smoke test, Autobahn and Hammer scripts
storage/examples/      carried over from upstream
```

Third-party dependencies (BoringSSL, Node headers) are not vendored. `src/native.ts` fetches them at the commits pinned at the top of that file.

## Build

The native addon and the TypeScript entry point are built separately.

Use Node.js 26 and pnpm 11.10.0. Published packages include six Node 26 ABI 147
binaries: Windows x64, macOS x64/ARM64, Linux glibc x64/ARM64, and Linux musl x64.
Install this private package with an npm account that has access:

```
pnpm add @esportsplus/uws@latest
```

Native addon requirements: clang 18+, cmake, ninja, git, Go (BoringSSL). Windows additionally needs the MSVC build tools environment and NASM.

```
pnpm build:native
```

Produces `dist/uws_<platform>_<arch>[_musl]_<abi>.node` for every Node version listed in `src/native.ts`. Local builds target the host architecture; macOS builds both x64 and ARM64.

The compiler and linker can be overridden with environment variables, for example when clang is not installed under the default name or when the system linker lacks the LLVM LTO plugin:

```
UWS_CC=clang UWS_CXX=clang++ UWS_LD=lld pnpm build:native
```

Linux is the primary development platform; the addon links with LTO there. The default Linux compiler commands are `clang-18` and `clang++-18`. Install the native build requirements above or override the compiler commands for your toolchain.

## Test

```
pnpm typecheck
pnpm test
pnpm smoke
```

`pnpm test` runs the vitest suite in `test/**/*.test.ts` against `dist/`, so the addon must be built first. The suite is the behavioural baseline of the fork point: tests whose names end in `(upstream quirk)` or `(upstream bug)` pin current behaviour on purpose and are updated deliberately when a roadmap item changes it.

## Bench

```
pnpm bench:http
pnpm bench:ws
```

Both spawn the server in a child process and print a markdown table. `bench:http` compares `uws` with `node:http` through autocannon; `bench:ws` floods echo sockets from worker threads. Options are passed as `--connections=`, `--pipelining=`, `--duration=`, `--workers=`, `--size=` and `--servers=`.

The published JavaScript entry point and declarations are emitted from `src/` into `build/`:

```
pnpm build
```

## Usage

```ts
import { App } from '@esportsplus/uws';

App().get('/*', (res) => { res.end('Hello World!'); }).listen(9001, () => {});
```

Async handlers must attach `res.onAborted` before returning and must not use `res` after it fires. The addon installs a default abort handler for a response that returns unresolved without one, solely to invalidate a retained `res` object safely; it does not make asynchronous response handling safe by itself.

This fork deliberately differs from uWebSockets.js v20.67.0 in a few API details:

- `res.tryEnd` returns packed flags rather than a destructurable tuple: bit 0 means the write succeeded and bit 1 means the response completed.
- `getRemoteAddressAsText` and `getProxiedRemoteAddressAsText` return strings.
- `res.collectBody` is available as a helper for collecting a request body.
- The experimental cached `get` overload and the descriptor/child-app APIs have been removed.

See `src/types.ts` for the current API surface.

## Request header size

`App()` and `SSLApp()` accept `maxHeaderSize`, the maximum size in bytes of one request's request line plus header block. It defaults to 16384 (Node's `--max-http-header-size` default) and is fixed at construction; requests over the limit are rejected with `431 Request Header Fields Too Large` and the connection is closed. A separate, fixed cap of 98 headers also returns `431` regardless of this value. `maxHeaderSize` is per app and is ignored when passed to `addServerName`, because SNI domains share the app's HTTP context. It supersedes the old `UWS_HTTP_MAX_HEADERS_SIZE` environment variable, which has been removed.

## Outbound WebSocket client

This fork adds an outbound client role alongside the server. `Client` dials `ws://`, `SSLClient` dials `wss://`, and both hand back the same `WebSocket` handler contract as an inbound socket (`open`, `message`, `drain`, `close`, `ping`, `pong`, `dropped`, `subscription`, plus `send`/`end`/`close`/`subscribe`/`publish`). A connection that never reaches `open` is reported once through `failed` with a `ConnectError`.

```ts
import { Client } from '@esportsplus/uws';

const client = Client();

client.connect('ws://127.0.0.1:9001', {
    open: (ws) => { ws.send('hello'); },
    message: (ws, message, isBinary) => { /* ... */ },
    failed: (error) => { console.error(error.code, error.message); }
});
```

`SSLClient` verifies the server certificate and hostname by default; when neither `ca_file_name` nor `ca_pem` is supplied it defaults `ca_pem` to Node's default CA set, including `NODE_EXTRA_CA_CERTS` (and the system store when Node is configured to use it), so ordinary public `wss://` endpoints work with no configuration. Pass `rejectUnauthorized: false` (per connect) or `reject_unauthorized: false` (per client) for self-signed development servers. `connectTimeout` and `handshakeTimeout` are milliseconds (both default to 10000). `close()` is terminal, frees the client on the next loop tick, and makes `connect()`, `publish()` and `numSubscribers()` throw `CLIENT_CLOSED` immediately. Creating one Client/SSLClient per reconnect is supported, though reusing one long-lived client remains cheaper. Reconnect and backoff are the application's responsibility — see `storage/examples/`. See `ClientOptions`, `ConnectBehavior` and `ConnectError` in `src/types.ts`.

An HTTP `CONNECT` proxy can be set per client (`Client({ proxy })`) or per connection (`connect(url, { proxy })`, which overrides the client default; pass `proxy: ''` to bypass it for one connection). The proxy URL is `http://[user:pass@]host:port` — `http://` only (no TLS to the proxy itself), port required; userinfo becomes a `Proxy-Authorization: Basic` header. For `wss://` through a proxy, TLS is negotiated end-to-end with the destination inside the tunnel, so certificate and hostname verification always target the destination, never the proxy. A `407` from the proxy surfaces as `failed` with `code: 'PROXY_AUTH'`; any other non-`200` CONNECT response as `code: 'PROXY_STATUS'`.

Connection-phase errors are delivered to `failed`; invalid URLs or headers, and `connect()` after `close()`, throw synchronously instead. Redirects are not followed. A 3xx WebSocket response arrives through `failed` with `code: 'HTTP_STATUS'` and its `headers.location`, so the caller can choose whether and how to reconnect.

Windows/macOS client error codes are validated by CI, not locally. A `Client` may be used inside a `worker_thread`, but tearing the worker down mid-connect blocks its exit until the pending DNS lookup returns, bounded by the OS resolver timeout.

## CI and releases

CI releases follow `push to main → bump version → build → publish to npm`.
The build workflow compiles and tests each native target once and runs ASAN. Publishing downloads
the binaries and exact source revision from that successful run; it does not repeat
native compilation or tests. To retry publishing manually, supply the successful
build run ID. To build and publish manually, dispatch build on main.

BoringSSL libraries are cached by platform and toolchain, including Alpine/musl.
Compiled addon binaries are also cached by native sources and headers, build script,
pinned Node headers/ABI, platform, compiler/linker, and build settings. ASAN uses a
separate cache. Exact hits skip compilation; binary verification and tests still
run. TypeScript-only, documentation, and test edits can reuse native binaries.
CI uses two isolated test workers and uploads test timing reports, with the slowest
tests listed in each platform job's summary. Slow network and timeout tests remain
part of the release checks; independent long HTTP/WebSocket timeouts overlap.
The smoke test finishes after verifying all three server and client closures,
with a deadline that fails if any expected closure is missing.

The `Extended tests` workflow runs full Autobahn server/client suites and benchmarks
nightly at 10:23 UTC, or on manual dispatch. These jobs share one native build and
do not block releases. Autobahn reports are uploaded even when a job fails.
Only one extended run may be active, and its server and client Autobahn suites run
one at a time. Pushes and completed version bumps cancel outdated extended runs
without starting new suites. Nightly/manual runs resolve the latest main commit
once and use that same revision for their native build, tests, and benchmarks.

The first root commit publishes `0.0.1` without a bump. Later pushes use the normal
version-bump rules. npm publishing uses GitHub OIDC; the npm token is used only for
private dependency installation. Package validation requires all six binaries.

## Platform notes

Linux is the primary, gated platform. `SO_REUSEPORT` lets several worker threads bind the same port and have the kernel distribute accepts.

Windows has no `SO_REUSEPORT` equivalent, so multiple workers on one port do not distribute accepts. Scale on Windows by running one port per worker behind a proxy.

This is an origin server: it answers `HTTP/1.0` requests and absolute-form request targets (`GET http://host/path`) with `505 HTTP Version Not Supported`.
