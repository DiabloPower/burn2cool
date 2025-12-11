#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${ROOT}/cpu_throttle
CTL=${ROOT}/cpu_throttle_ctl
TEST_SCRIPT=${ROOT}/tests/test_cli_excluded_types.sh

echo "Integration tests: build binaries and run daemon"
cd "$ROOT"
make -j2

# Start daemon in background as non-root if no socket exists
STARTED_DAEMON=0
if [ -S /tmp/cpu_throttle.sock ]; then
  echo "/tmp/cpu_throttle.sock already exists; using present daemon for testing"
else
  echo "Starting daemon in background"
  "$BIN" --web-port --quiet &
  PID=$!
  STARTED_DAEMON=1
  echo "daemon pid=$PID"
  trap 'echo "Stopping daemon..."; kill $PID || true; wait $PID 2>/dev/null || true' EXIT
fi

echo "Waiting for daemon to accept socket..."
timeout=20
while [[ $timeout -gt 0 ]]; do
  if [ -S /tmp/cpu_throttle.sock ]; then
    break
  fi
  sleep 1; timeout=$((timeout-1))
done
if [[ $timeout -eq 0 ]]; then
  echo "Daemon didn't create socket in time"; exit 2
fi

echo "Running CLI test script..."
chmod +x "$TEST_SCRIPT"
"$TEST_SCRIPT"

echo "Running HTTP excluded-types tests..."
chmod +x "$ROOT/tests/test_http_excluded_types.sh"
"$ROOT/tests/test_http_excluded_types.sh"
echo "Running normalization tests..."
chmod +x "$ROOT/tests/test_normalize_excluded_types.sh"
"$ROOT/tests/test_normalize_excluded_types.sh"

echo "Integration tests passed"
exit 0
