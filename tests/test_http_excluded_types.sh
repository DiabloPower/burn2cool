#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
API_URL=http://127.0.0.1:8086

# Helper
function do() { echo "$@"; "$@"; }

# ensure server is up via /api/status
for i in {1..10}; do
  if curl -sSf "$API_URL/api/status" >/dev/null 2>&1; then break; fi
  sleep 1
done

# set excluded types using POST payload with varied whitespace and quotes
BODY='{"value":" WiFi , INT3400  "}'
RESP=$(curl -sS -X POST -H 'Content-Type: application/json' -d "$BODY" "$API_URL/api/settings/excluded-types")
if [[ "$RESP" != *"saved"* ]]; then
  echo "HTTP: POST failed or didn't indicate saved: $RESP"; exit 1
fi

# GET and ensure normalization: lower-case and trimmed
GETRESP=$(curl -sS "$API_URL/api/settings/excluded-types")
if [[ "$GETRESP" != *"wifi"* || "$GETRESP" != *"int3400"* ]]; then
  echo "HTTP: GET doesn't contain normalized tokens: $GETRESP"; exit 2
fi

echo "HTTP excluded-types tests passed"
exit 0
