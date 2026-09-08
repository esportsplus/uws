#!/usr/bin/env bash
set -euo pipefail

# Slim remote harness for building/testing the native addon on a bare Linux box
# (the Geekom AE8). Adapted from the lmdbx-js remote.sh — only the shared
# bare-ssh verbs are kept (no DigitalOcean lifecycle, bench, soak or certify).
# Run from WSL, which has rsync + ssh; Git Bash lacks rsync.

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
ENV_FILE="$PROJECT_DIR/.remote.env"

# clang + lld toolchain (BoringSSL fails to build under gcc's -Werror
# stringop-overflow). CC/CXX steer BoringSSL's cmake to clang too, since
# native.ts's Linux cmake call doesn't pin a compiler. MAKEFLAGS
# parallelises BoringSSL's cmake make.
BUILD_ENV="UWS_CC=clang UWS_CXX=clang++ UWS_LD=lld CC=clang CXX=clang++ MAKEFLAGS=-j\$(nproc)"

load_env() {
    if [[ ! -f "$ENV_FILE" ]]; then
        echo "ERROR: .remote.env not found at $ENV_FILE" >&2
        exit 1
    fi

    source "$ENV_FILE"

    for var in REMOTE_HOST REMOTE_USER REMOTE_DIR SSH_KEY; do
        if [[ -z "${!var:-}" ]]; then
            echo "ERROR: $var is not set in .remote.env" >&2
            exit 1
        fi
    done

    SSH_PORT="${SSH_PORT:-22}"
    SSH_OPTS=(
        -i "$SSH_KEY"
        -p "$SSH_PORT"
        -o StrictHostKeyChecking=accept-new
        -o ConnectTimeout=10
        -o ServerAliveInterval=15
        -o ServerAliveCountMax=4
    )
}

ssh_cmd() {
    ssh "${SSH_OPTS[@]}" "$REMOTE_USER@$REMOTE_HOST" "$@"
}

ssh_tty() {
    ssh -t "${SSH_OPTS[@]}" "$REMOTE_USER@$REMOTE_HOST" "$@"
}

# --- Remote exclusive lock ---
# The box is a single-writer resource shared across sessions and machines; two
# sync -> build -> test windows interleaving clobber each other's tree. The
# mutating verbs acquire an atomic remote lockdir for the whole invocation.
REMOTE_LOCK_PATH="/tmp/uws-remote.lock.d"
REMOTE_LOCK_STALE_SECS="${REMOTE_LOCK_STALE_SECS:-1800}"
REMOTE_LOCK_WAIT_SECS="${REMOTE_LOCK_WAIT_SECS:-300}"
REMOTE_LOCK_HELD=0

acquire_remote_lock() {
    [[ "${REMOTE_NO_LOCK:-0}" == "1" ]] && return 0

    local holder waited=0 owner age
    holder="$(hostname 2>/dev/null || echo host)/${USER:-user}/pid$$/$(date -u +%Y%m%dT%H%M%SZ)"

    while true; do
        if ssh_cmd "mkdir '$REMOTE_LOCK_PATH' 2>/dev/null && printf '%s' '$holder' > '$REMOTE_LOCK_PATH/holder'"; then
            REMOTE_LOCK_HELD=1
            return 0
        fi

        owner="$(ssh_cmd "cat '$REMOTE_LOCK_PATH/holder' 2>/dev/null" || true)"
        age="$(ssh_cmd "echo \$(( \$(date +%s) - \$(stat -c %Y '$REMOTE_LOCK_PATH' 2>/dev/null || echo 0) ))" 2>/dev/null || echo 0)"

        if [[ "${age:-0}" -gt "$REMOTE_LOCK_STALE_SECS" ]]; then
            echo "remote-lock: stealing STALE lock (age ${age}s, holder ${owner:-unknown})" >&2
            ssh_cmd "rm -rf '$REMOTE_LOCK_PATH'" || true
            continue
        fi

        if [[ "$waited" -ge "$REMOTE_LOCK_WAIT_SECS" ]]; then
            echo "ERROR: box is held by another session (holder=${owner:-unknown}, age=${age}s)." >&2
            echo "  Serialize remote access; or clear a dead lock: bash scripts/remote.sh unlock" >&2
            exit 1
        fi

        echo "remote-lock: waiting for box (holder=${owner:-unknown}, ${waited}s/${REMOTE_LOCK_WAIT_SECS}s)..." >&2
        sleep 10
        waited=$((waited + 10))
    done
}

release_remote_lock() {
    [[ "${REMOTE_LOCK_HELD:-0}" == "1" ]] || return 0
    ssh_cmd "rm -rf '$REMOTE_LOCK_PATH'" 2>/dev/null || true
    REMOTE_LOCK_HELD=0
}

with_remote_lock() {
    acquire_remote_lock
    trap release_remote_lock EXIT
    "$@"
    release_remote_lock
    trap - EXIT
}

do_unlock() {
    local owner
    owner="$(ssh_cmd "cat '$REMOTE_LOCK_PATH/holder' 2>/dev/null" || true)"
    ssh_cmd "rm -rf '$REMOTE_LOCK_PATH'" && echo "remote-lock: cleared (was: ${owner:-none})"
}

do_lock_status() {
    local owner age
    if owner="$(ssh_cmd "cat '$REMOTE_LOCK_PATH/holder' 2>/dev/null")" && [[ -n "$owner" ]]; then
        age="$(ssh_cmd "echo \$(( \$(date +%s) - \$(stat -c %Y '$REMOTE_LOCK_PATH' 2>/dev/null || echo 0) ))" 2>/dev/null || echo '?')"
        echo "remote-lock: HELD by $owner (age ${age}s)"
    else
        echo "remote-lock: free"
    fi
}

do_sync() {
    echo "--- Syncing to $REMOTE_USER@$REMOTE_HOST:$REMOTE_DIR ---" >&2
    ssh_cmd "mkdir -p '$REMOTE_DIR'"
    rsync -az --delete \
        --exclude='.git/' \
        --exclude='node_modules/' \
        --exclude='build/' \
        --exclude='dist/' \
        --exclude='deps/' \
        --exclude='.remote.env' \
        --exclude='.claude/' \
        -e "ssh ${SSH_OPTS[*]}" \
        "$PROJECT_DIR/" \
        "$REMOTE_USER@$REMOTE_HOST:$REMOTE_DIR/"
    echo "--- Sync complete ---" >&2
}

do_install() {
    # esbuild ships its binary via a platform optionalDep, so its ignored build
    # script is harmless; treat a lone ERR_PNPM_IGNORED_BUILDS as success.
    ssh_cmd "cd '$REMOTE_DIR' && out=\$(pnpm install --prefer-offline 2>&1); c=\$?; printf '%s\n' \"\$out\"; if [ \$c -ne 0 ]; then printf '%s' \"\$out\" | grep -q ERR_PNPM_IGNORED_BUILDS || exit \$c; fi"
}

do_build() {
    local extra="${1:-}"
    ssh_cmd "cd '$REMOTE_DIR' && $BUILD_ENV node src/native.ts $extra"
}

do_build_full() {
    do_sync
    do_build
}

do_build_asan() {
    do_sync
    do_build --asan
}

do_test() {
    do_sync
    do_build
    echo "--- Running tests ---" >&2

    local test_cmd="UWS_NET_TESTS=1 UWS_SLOW_TESTS=1 node_modules/.bin/vitest run"
    if [[ $# -gt 0 ]]; then
        test_cmd="UWS_NET_TESTS=1 UWS_SLOW_TESTS=1 node_modules/.bin/vitest run $*"
    fi

    ssh_cmd "cd '$REMOTE_DIR' && $test_cmd"
}

do_test_asan() {
    do_sync
    do_build --asan
    echo "--- Running tests under ASAN ---" >&2

    # Node is not ASAN-instrumented, so preload the clang ASAN runtime; disable
    # leak detection (Node leaks intentionally at exit) but keep error aborts.
    local asan_rt
    asan_rt="$(ssh_cmd 'clang -print-file-name=libclang_rt.asan-x86_64.so')"

    local test_cmd="UWS_NET_TESTS=1 UWS_SLOW_TESTS=1 node_modules/.bin/vitest run"
    if [[ $# -gt 0 ]]; then
        test_cmd="UWS_NET_TESTS=1 UWS_SLOW_TESTS=1 node_modules/.bin/vitest run $*"
    fi

    ssh_cmd "cd '$REMOTE_DIR' && LD_PRELOAD='$asan_rt' ASAN_OPTIONS=detect_leaks=0:abort_on_error=1:handle_segv=1 $test_cmd"
}

do_autobahn() {
    do_sync
    do_build
    echo "--- Running Autobahn fuzzingclient (Docker) against a uWS echo server ---" >&2

    # Single-quoted heredoc: everything expands on the box. Reports land outside
    # the synced tree so rsync --delete never touches them.
    ssh_cmd "bash -s" <<'REMOTE'
set -uo pipefail
cd "$HOME/uws"
REPORTS="$HOME/uws-autobahn-reports"
rm -rf "$REPORTS"; mkdir -p "$REPORTS"

node --import tsx test/autobahn/echo-server.ts > /tmp/uws-echo.log 2>&1 &
ECHO_PID=$!
trap 'kill $ECHO_PID 2>/dev/null || true' EXIT

for i in $(seq 1 40); do
    timeout 1 bash -c '</dev/tcp/127.0.0.1/9001' 2>/dev/null && break
    sleep 0.5
done

docker run --rm --network host \
    -v "$PWD/test/autobahn":/config:ro \
    -v "$REPORTS":/reports \
    crossbario/autobahn-testsuite \
    wstest -m fuzzingclient -s /config/fuzzingclient.json

kill $ECHO_PID 2>/dev/null || true

echo "--- echo server log (tail) ---"
tail -5 /tmp/uws-echo.log || true

node --import tsx test/autobahn/report.ts "$REPORTS/index.json"
REMOTE
}

# Client role: run the Autobahn fuzzingserver in Docker (listens on 9001) and drive it with our
# uWS.Client echo-client. The server writes the report under clients/index.json on updateReports.
do_autobahn_client() {
    do_sync
    do_build
    echo "--- Running Autobahn fuzzingserver (Docker) against the uWS client ---" >&2

    ssh_cmd "bash -s" <<'REMOTE'
set -uo pipefail
cd "$HOME/uws"
REPORTS="$HOME/uws-autobahn-client-reports"
rm -rf "$REPORTS"; mkdir -p "$REPORTS"

docker rm -f uws-fuzzingserver >/dev/null 2>&1 || true
docker run --rm --name uws-fuzzingserver --network host \
    -v "$PWD/test/autobahn":/config:ro \
    -v "$REPORTS":/reports \
    crossbario/autobahn-testsuite \
    wstest -m fuzzingserver -s /config/fuzzingserver.json > /tmp/uws-fuzzingserver.log 2>&1 &
trap 'docker kill uws-fuzzingserver >/dev/null 2>&1 || true' EXIT

for i in $(seq 1 40); do
    timeout 1 bash -c '</dev/tcp/127.0.0.1/9001' 2>/dev/null && break
    sleep 0.5
done

node --import tsx test/autobahn/echo-client.ts

# The server flushes the report on the updateReports request; give it a moment.
sleep 1

echo "--- fuzzingserver log (tail) ---"
tail -5 /tmp/uws-fuzzingserver.log || true

node --import tsx test/autobahn/report.ts "$REPORTS/index.json"
REMOTE
}

# Build + run the reps/median bench on the box, pinned off the OS cores. Sync and
# build noise goes to stderr; ONLY the run.ts JSON reaches stdout, so callers can
# capture it with command substitution.
do_bench() {
    local reps="${1:-5}"
    do_sync >&2
    do_build >&2
    echo "--- Running benchmark (reps=$reps) ---" >&2
    ssh_cmd "cd '$REMOTE_DIR' && taskset -c 2-\$(( \$(nproc) - 1 )) node --import tsx bench/run.ts --json --reps=$reps"
}

# A/B a metric across two git refs: bench each on the box, diff on the box.
do_bench_compare() {
    local refA="$1" refB="$2" reps="${3:-5}" orig jsonA jsonB

    if [[ -z "$refA" || -z "$refB" ]]; then
        echo "Usage: remote.sh bench:compare <refA> <refB> [reps]" >&2
        exit 1
    fi
    if [[ -n "$(git -C "$PROJECT_DIR" status --porcelain)" ]]; then
        echo "ERROR: working tree not clean; commit or stash before bench:compare" >&2
        exit 1
    fi

    orig="$(git -C "$PROJECT_DIR" symbolic-ref -q --short HEAD || git -C "$PROJECT_DIR" rev-parse HEAD)"
    trap 'git -C "$PROJECT_DIR" checkout -q "$orig"' EXIT

    echo "=== A: $refA ===" >&2
    git -C "$PROJECT_DIR" checkout -q "$refA"
    jsonA="$(do_bench "$reps")"

    echo "=== B: $refB ===" >&2
    git -C "$PROJECT_DIR" checkout -q "$refB"
    jsonB="$(do_bench "$reps")"

    git -C "$PROJECT_DIR" checkout -q "$orig"
    trap - EXIT

    do_sync >&2
    printf '%s' "$jsonA" | ssh_cmd "cat > /tmp/uws-bench-A.json"
    printf '%s' "$jsonB" | ssh_cmd "cat > /tmp/uws-bench-B.json"
    ssh_cmd "cd '$REMOTE_DIR' && node --import tsx bench/compare.ts /tmp/uws-bench-A.json /tmp/uws-bench-B.json"
}

case "${1:-help}" in
    sync)        load_env; with_remote_lock do_sync ;;
    install)     load_env; with_remote_lock do_install ;;
    build)       load_env; with_remote_lock do_build_full ;;
    build:asan)  load_env; with_remote_lock do_build_asan ;;
    test)        load_env; shift; with_remote_lock do_test "$@" ;;
    test:asan)   load_env; shift; with_remote_lock do_test_asan "$@" ;;
    autobahn)    load_env; with_remote_lock do_autobahn ;;
    autobahn:client) load_env; with_remote_lock do_autobahn_client ;;
    bench)       load_env; shift; with_remote_lock do_bench "${1:-5}" ;;
    bench:compare) load_env; shift; with_remote_lock do_bench_compare "$@" ;;
    run)         load_env; shift; ssh_tty "cd '$REMOTE_DIR' && $*" ;;
    shell)       load_env; ssh_tty "cd '$REMOTE_DIR' && exec bash" ;;
    unlock)      load_env; do_unlock ;;
    lock:status) load_env; do_lock_status ;;
    help|*)
        echo "Usage: remote.sh <sync|install|build|build:asan|test [files]|test:asan [files]|autobahn|autobahn:client|bench [reps]|bench:compare <refA> <refB> [reps]|run <cmd>|shell|unlock|lock:status>"
        ;;
esac
