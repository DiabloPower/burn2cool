#!/usr/bin/env bash
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${ROOT}/cpu_throttle_ctl
API_URL=http://127.0.0.1:8086

echo "Testing normalization via CLI and HTTP"
${BIN} set-excluded-types none
${BIN} set-excluded-types --merge " WiFi ,   INT3400 "
res=$(${BIN} get-excluded-types 2>/dev/null || true)
if [[ "$res" != *"wifi"* || "$res" != *"int3400"* ]]; then
  echo "CLI normalization failed: $res"; exit 1
fi
echo "CLI normalization: PASS"

if curl -sSf "$API_URL/api/settings/excluded-types" >/dev/null 2>&1; then
  curl -sS -X POST -H 'Content-Type: application/json' -d '{"value":"  WiFi ,   INT3401  "}' "$API_URL/api/settings/excluded-types" >/dev/null
  getres=$(curl -sS "$API_URL/api/settings/excluded-types")
  if [[ "$getres" != *"wifi"* || "$getres" != *"int3401"* ]]; then
    echo "HTTP normalization failed: $getres"; exit 1
  fi
  echo "HTTP normalization: PASS"
else
  echo "HTTP API is not reachable, skipping HTTP normalization tests"
fi

echo "Normalization tests passed"
exit 0
