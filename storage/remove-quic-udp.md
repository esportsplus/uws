# Remove HTTP/3, QUIC and UDP

Decision: the upstream HTTP/3 stack is removed from the fork in full. Nothing is kept behind a build flag.

Why. The upstream `H3App` is a 2022 smoke test that was never finished: the H3 request object cannot read its own URL or method from JavaScript, `quic.c` has 69 unconditional `printf` calls, `Http3Context.h` prints on every stream open and close, and UDP send failures print "unhandled udp backpressure!" and continue. No browser speaks WebSocket over HTTP/3 and this build exposes no WebTransport, so the code serves no client today. It is also the only reason the build needs lsquic, zlib and msbuild. The protocol itself is mature and worth adding back properly; see the last section.

There is no standalone UDP feature to preserve. `udp.c` is consumed only by `quic.c`, and no UDP API reaches the addon or JavaScript.

## Inventory

Files deleted outright:

| File | Lines |
|---|---|
| `native/usockets/quic.c` | 797 |
| `native/usockets/quic.h` | 47 |
| `native/usockets/udp.c` | 112 |
| `native/uwebsockets/Http3App.h` | 116 |
| `native/uwebsockets/Http3Context.h` | 117 |
| `native/uwebsockets/Http3ContextData.h` | 16 |
| `native/uwebsockets/Http3Request.h` | 19 |
| `native/uwebsockets/Http3Response.h` | 82 |
| `native/uwebsockets/Http3ResponseData.h` | 19 |
| `examples/H3lloWorld.ts` | 23 |

Files edited: `native/usockets/bsd.c`, `native/usockets/internal/networking/bsd.h`, `native/usockets/libusockets.h`, `native/usockets/internal/internal.h`, `native/usockets/loop.c`, `native/usockets/eventing/libuv.c`, `native/usockets/eventing/gcd.c`, `native/usockets/eventing/epoll_kqueue.c`, `native/uwebsockets/CachingApp.h`, `native/addon/addon.cpp`, `native/addon/Utilities.h`, `native/addon/AppWrapper.h`, `native/addon/HttpRequestWrapper.h`, `native/addon/HttpResponseWrapper.h`, `src/native.ts`, `src/index.ts`, `src/types.ts`, `README.md`, `ROADMAP.md`.

Roughly 1,850 lines removed. `misc/cert.pem` and `misc/key.pem` stay; every TLS example uses them.

## Step 1: uSockets

Delete `quic.c`, `quic.h`, `udp.c`.

`native/usockets/bsd.c`:

- Delete line 20, `#define __APPLE_USE_RFC_3542`. It only enables the IPv6 packet-info options used by the UDP socket.
- Delete lines 41 to 267: `struct us_internal_udp_packet_buffer`, `bsd_sendmmsg`, `bsd_recvmmsg`, `bsd_udp_packet_buffer_local_ip`, `bsd_udp_packet_buffer_peer`, `bsd_udp_packet_buffer_payload`, `bsd_udp_packet_buffer_payload_length`, `bsd_udp_buffer_set_packet_payload`, `bsd_create_udp_packet_buffer`.
- Delete lines 586 to 710: `bsd_create_udp_socket` and `bsd_udp_packet_buffer_ecn`.
- Leave the commented `//#define _GNU_SOURCE` on line 29 alone; it is dead either way and Phase 1 of the roadmap strips commented code.

`native/usockets/internal/networking/bsd.h`:

- Delete lines 47 and 48, `LIBUS_UDP_MAX_SIZE` and `LIBUS_UDP_MAX_NUM`.
- Delete lines 58 to 66, the nine `bsd_*mmsg` and `bsd_udp_*` prototypes.
- Delete lines 102 and 103, `bsd_create_udp_socket` and its comment.
- Keep the `_GNU_SOURCE` define on lines 38 and 39. Other glibc extensions in `bsd.c` may rely on it; verify separately if desired.

`native/usockets/libusockets.h`:

- Delete lines 61 and 62, the forward declarations of `us_udp_socket_t` and `us_udp_packet_buffer_t`.
- Delete lines 67 to 108, the whole "Public interface for UDP sockets" block including the two commented-out `us_create_udp_socket` prototypes.
- Keep lines 64 and 65 (`us_socket_send_buffer`); that belongs to the io_uring removal in Phase 1.

`leave_poll_ready`. The only writer that sets it to 1 is `udp.c:127`. After removal it is always zero, so delete the field and its readers:

- `native/usockets/internal/internal.h:109`, the field.
- `native/usockets/loop.c:229` to `:235`: drop the comment and the `if (!cb->leave_poll_ready)` guard, keep the `#ifndef LIBUS_USE_LIBUV` accept call as is.
- `native/usockets/eventing/libuv.c:217`, `gcd.c:200`, `gcd.c:254`, `epoll_kqueue.c:300`, `:310`, `:381`, `:416`: delete the assignments. The gcd and epoll files are scheduled for deletion in Phase 1; the edits here keep them compiling until then.

## Step 2: uWebSockets

Delete the six `Http3*.h` headers. Nothing under `native/uwebsockets/` includes them except each other; `App.h` does not reference HTTP/3.

`native/uwebsockets/CachingApp.h:58`: delete the comment `// we can also derive from H3app later on`.

## Step 3: addon

`native/addon/addon.cpp`:

- Delete line 20, `#include "Http3App.h"`.
- Delete lines 393 and 394, the `H3App` export.
- Lines 377 and 378 become one line: `perContextData->reqTemplate.Reset(isolate, HttpRequestWrapper::init(isolate));`.
- Lines 379 to 382: keep `init<0>` and `init<1>`, delete the `init<2>` line, and change the cache line to `resTemplate[2].Reset(isolate, HttpResponseWrapper::init<2>(isolate))`.

`native/addon/Utilities.h`:

- Line 76: `reqTemplate[2]` becomes a single `UniquePersistent<Object> reqTemplate;`.
- Line 77: `resTemplate[4]` becomes `resTemplate[3]` with the comment `0 = non-SSL, 1 = SSL, 2 = CACHE`.
- Lines 85 to 102, `getAppTypeIndex`: replace the body with `return std::is_same<APP, uWS::SSLApp>::value;` (the line already present in a comment on line 88) and drop the stale comments.

`native/addon/HttpRequestWrapper.h`. The whole file is templated on `int QUIC` for one cast. Remove the parameter everywhere:

- `getHttpRequest` (line 28) loses its template and the `if constexpr (QUIC)` cast on lines 37 to 41; it returns `req`.
- Every `template <int QUIC>` on the `req_*` functions goes, and every `getHttpRequest<QUIC>(args)` becomes `getHttpRequest(args)`.
- `init` (line 181) loses its template. Line 185 sets the class name to `"uWS.HttpRequest"` unconditionally. Lines 191 and 199 (the `if constexpr (!QUIC)` guard) go; the seven registrations inside stay.

`native/addon/HttpResponseWrapper.h`. `PROTOCOL` is 0 = TCP, 1 = TLS, 2 = QUIC, 3 = CACHE. QUIC goes and CACHE moves to 2:

- Line 26 comment: `0 = TCP, 1 = TLS, 2 = CACHE`.
- Lines 44 to 51: delete the `PROTOCOL == 2` branch; the cache branch tests `PROTOCOL == 2`.
- Line 581 comment: same renumbering.
- Lines 588 to 593: delete the `SSL == 2` branch that names the class `uWS.Http3Response`; the cache branch tests `SSL == 2`.
- Line 600: `SSL != 3` becomes `SSL != 2`.
- Lines 611 and 612: delete the comment `QUIC has a lot of functions unimplemented` and the `if constexpr (SSL != 2)` guard. The sixteen registrations inside (lines 613 to 627) stay, and the closing brace on line 628 goes.

`native/addon/AppWrapper.h`:

- Lines 134, 475, 973: `reqTemplate[std::is_same<APP, uWS::H3App>::value]` becomes `reqTemplate`.
- Line 344: delete `if constexpr (!std::is_same<APP, uWS::H3App>::value) {` and its matching closing brace; the DeclarativeResponse body stays.
- Lines 912 to 929: keep the body of the first branch (construct, check `constructorFailed`, throw) and delete the `else` branch that sets the class name `uWS.H3App`.
- Line 969: `resTemplate[/*getAppTypeIndex<APP>()*/3]` becomes `resTemplate[2]`. This is the cached-get experiment that Phase 1 deletes; the index must move now so the array shrink compiles.
- Lines 1040 and 1073: delete the two `if constexpr (!std::is_same<APP, uWS::H3App>::value)` guards and their closing braces; the bodies stay.

After this step `grep -rn "H3App\|Http3\|QUIC" native/addon` returns nothing.

## Step 4: native build script

`src/native.ts`:

- `Dependency` type: drop `version`, make `commit` required.
- `Libraries` type: drop `lsquic`.
- `DEPS`: delete the `lsquic` and `zlib` entries.
- `compile`: delete the lsquic include on line 92, the `-DLIBUS_USE_QUIC` define on line 95, and the Windows `wincompat` include on lines 106 to 108.
- `fetch`: the loop on line 125 iterates `['boringssl']` only. Delete the zlib download on lines 136 to 139.
- `link`: remove `"${libs.lsquic}"` from all three link lines.
- Delete the `lsquic` function (lines 171 to 190) and the call on line 232.

msbuild is no longer invoked anywhere. NASM stays: BoringSSL uses it for assembly on Windows. `ilammy/msvc-dev-cmd` in `.github/workflows/build.yml` also stays, since clang on Windows needs the MSVC SDK environment it provides. No CI change.

Locally, delete `deps/lsquic` and `deps/zlib-1.3.1` (both untracked) before the verification build so the build proves it no longer needs them.

## Step 5: TypeScript surface

- `src/index.ts`: delete line 79, `const H3App = uws.H3App;`, and `H3App,` from the export list on line 193.
- `src/types.ts`: delete line 300, `H3App(options?: AppOptions): TemplatedApp;`.
- Delete `examples/H3lloWorld.ts`.

This is a breaking API change for anyone importing `H3App`. Nobody can be, since it never worked past a hello world, but the commit is marked breaking.

## Step 6: docs

`README.md`:

- Line 26: `Third-party dependencies (BoringSSL, Node headers) are not vendored.`
- Line 32: `Windows additionally needs the MSVC build tools environment and NASM.`

`ROADMAP.md`:

- Line-count table: replace the HTTP/3 row with a note that it was removed, or drop the row and adjust the total.
- Phase 0 dependency table: delete the lsquic and zlib rows, and drop "Rebuild lsquic against it" from the BoringSSL row.
- Phase 0 "Decision required: HTTP/3" paragraph: replace with one line pointing at this document.
- Phase 1 "Strip commented-out code" bullet: drop `quic.c` from the list.

## Verification

Run in this order; every step must pass before the next.

1. `grep -rniE "quic|udp|http3|h3app|lsquic|mmsghdr" --exclude-dir=.git --exclude-dir=node_modules --exclude-dir=deps --exclude-dir=storage --exclude=pnpm-lock.yaml .` The only surviving hits are `native/uwebsockets/ProxyParser.h:35` and `:41` (PROXY protocol v2 wording, keep), the word "quick" in `WebSocketExtensions.h:80`, and "quickly" in `libuv.c:47` and `:103`.
2. `tsc --noEmit -p tsconfig.json` clean.
3. `pnpm build:native` on the current platform, then on all four CI runners via the workflow. Windows must pass without msbuild on the path.
4. `node test/smoke.ts`.
5. `pnpm test` and `pnpm bench`; request throughput on the `res.end` route must match the baseline in `ROADMAP.md` within noise. Nothing in the TCP path changed, so any drift is a regression.
6. `du -sh dist/*.node` before and after for the record; expect a smaller binary.

## Commits

Two commits, both building on their own:

1. `refactor!: remove HTTP/3, QUIC and UDP` covering steps 1 to 5. Body states the `H3App` export is gone and why.
2. `docs: drop HTTP/3 from README and ROADMAP` covering step 6.

## When QUIC comes back

The protocol is worth having: WebTransport is Baseline in every browser as of Safari 26.4, and it gives browsers unreliable datagrams and connection migration that WebSocket cannot. Nothing in this removal is a design input to that work, which is why the code is not kept.

Decided direction for the rebuild, recorded here so the reasoning is not lost:

- Engine: ngtcp2 plus nghttp3 on BoringSSL. Both are sans-I/O, so uSockets keeps ownership of the UDP socket and the loop. nghttp3 already implements the HTTP datagram and capsule protocol (RFC 9297), extended CONNECT, priorities (RFC 9218) and WebSocket over HTTP/3 (RFC 9220), which is the full WebTransport substrate. Node core uses the same pair.
- Rejected: msquic and quinn own their threads and would add a cross-thread hop per packet. quiche is a close second but needs a Rust toolchain in the native build.
- Shape: one `listen` binds TCP and UDP on the same port and advertises `Alt-Svc`. HTTP/3 request and response objects reach full parity with HTTP/1. A new `app.wt()` mirrors `app.ws()` for WebTransport sessions, streams and datagrams. A client helper races WebTransport against WebSocket behind one interface. Nothing is flagged; browsers fall back to TCP on their own when UDP is blocked.
- Build order: transport plus plain HTTP/3 verified with curl, then WebTransport, then the client helper, then RFC 9220 wiring for when browsers ship it.

That work starts from a design note, not from the deleted files.
