#!/bin/sh
# Apply ngtcp2 undersized-Initial patch only when stock ngtcp2 is present (idempotent).
set -e
srcdir="$1"
patchfile="$2"
cd "$srcdir" || exit 1
if grep -q 'dgramlen < NGTCP2_MAX_UDP_PAYLOAD_SIZE' lib/ngtcp2_conn.c; then
  patch -p1 -i "$patchfile"
fi
