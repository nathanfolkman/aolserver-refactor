#!/usr/bin/env bash
# Optional reproducer for intermittent TLS/HTTP/2 crashes under load.
# Prerequisite: nsd already listening on HTTPS (e.g. minimal.tcl on 127.0.0.1:8443).
#
# Usage (from repo root, after exports from README.md):
#   tests/h2test/stress-parallel-h2.sh          # 40 parallel curls
#   tests/h2test/stress-parallel-h2.sh 200    # custom count
#
# If the server segfaults, capture a core / run under lldb:
#   lldb -- ./build/nsd/nsd -f -t tests/h2test/minimal.tcl
#   (lldb) run
#   # in another terminal, run this script
#   (lldb) bt all
#
# ASan build: see README.md "Debugging with AddressSanitizer / lldb".

set -uo pipefail
HOST="${H2SPEC_HOST:-127.0.0.1}"
PORT="${H2SPEC_PORT:-8443}"
N="${1:-40}"
URL="https://${HOST}:${PORT}/"

echo "stress-parallel-h2: ${N} parallel curl --http2 -> ${URL}"
for ((i = 0; i < N; i++)); do
  curl -sk --http2 --max-time 15 "$URL" -o /dev/null &
done
wait
echo "stress-parallel-h2: done"
