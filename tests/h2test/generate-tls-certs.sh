#!/usr/bin/env bash
# Create self-signed cert.pem + key.pem for tests/h2test/minimal.tcl (and h3test, which
# reuses this directory). Do not commit PEM files — they are gitignored.
#
# Usage: from repo root or any cwd:
#   tests/h2test/generate-tls-certs.sh
# Idempotent: skips if both files already exist unless H2TEST_TLS_REGEN=1.

set -eo pipefail
HERE="$(cd "$(dirname "$0")" && pwd)"
CERTDIR="$HERE/servers/s1/modules/nsssl"
mkdir -p "$CERTDIR"
CERT="$CERTDIR/cert.pem"
KEY="$CERTDIR/key.pem"

if [[ -f "$CERT" && -f "$KEY" && -z "${H2TEST_TLS_REGEN:-}" ]]; then
  exit 0
fi

if ! command -v openssl >/dev/null 2>&1; then
  echo "generate-tls-certs: openssl not found (install OpenSSL)" >&2
  exit 2
fi

openssl req -x509 -newkey rsa:2048 -sha256 -days 3650 -nodes \
  -keyout "$KEY" -out "$CERT" \
  -subj "/CN=localhost"
chmod 600 "$KEY"
chmod 644 "$CERT"
