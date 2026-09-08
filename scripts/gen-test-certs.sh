#!/usr/bin/env bash
set -euo pipefail

# Keep Git Bash from interpreting OpenSSL distinguished names as filesystem paths.
export MSYS2_ARG_CONV_EXCL='/CN='

# Generate the test TLS material into a throwaway directory (default .tmp/, which
# is gitignored) so no certificates are committed. Run automatically by the
# vitest globalSetup (test/setup/certs.ts) and available manually via `pnpm certs`.
#
#   - cert.pem / key.pem: a generic self-signed leaf (unencrypted key) used by the
#     plain SSLApp tests and every SSL example.
#   - test-ca/*: a tiny PKI (CA + localhost + loopback leaves) used by the
#     CA-verifying client TLS tests.
output_dir="${1:-.tmp}"
ca_dir="$output_dir/test-ca"
mkdir -p "$ca_dir"

# --- Generic self-signed leaf (cert.pem / key.pem) ---
# -nodes keeps key.pem an unencrypted PKCS8 "BEGIN PRIVATE KEY"; test/ssl.test.ts
# asserts a passphrase is ignored for it.
openssl req -x509 -newkey rsa:2048 -nodes -sha256 -days 36525 \
    -subj '/CN=localhost' \
    -addext 'subjectAltName=DNS:localhost,IP:127.0.0.1,IP:::1' \
    -keyout "$output_dir/key.pem" -out "$output_dir/cert.pem"

# --- Test PKI (test-ca/) ---
openssl genrsa -out "$ca_dir/ca.key" 2048
openssl req -x509 -new -key "$ca_dir/ca.key" -sha256 -days 36525 \
    -subj '/CN=Test CA' -out "$ca_dir/ca.pem"

make_leaf() {
    local name="$1"
    local subject="$2"
    local san="$3"
    local extfile

    extfile="$(mktemp)"
    trap 'rm -f "$extfile"' RETURN
    printf '%s\n' \
        'basicConstraints=critical,CA:FALSE' \
        'keyUsage=critical,digitalSignature,keyEncipherment' \
        'extendedKeyUsage=serverAuth' \
        "subjectAltName=$san" > "$extfile"

    openssl genrsa -out "$ca_dir/$name.key" 2048
    openssl req -new -key "$ca_dir/$name.key" -subj "$subject" -out "$ca_dir/$name.csr"
    openssl x509 -req -in "$ca_dir/$name.csr" -CA "$ca_dir/ca.pem" -CAkey "$ca_dir/ca.key" \
        -CAcreateserial -sha256 -days 36525 -extfile "$extfile" -out "$ca_dir/$name.pem"
    rm -f "$ca_dir/$name.csr"
    trap - RETURN
}

make_leaf localhost '/CN=localhost' 'DNS:localhost'
make_leaf loopback '/CN=loopback' 'IP:127.0.0.1,IP:::1,DNS:localhost'
rm -f "$ca_dir/ca.srl"
