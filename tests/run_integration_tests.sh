#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${ROOT}/cpu_throttle
CTL=${ROOT}/cpu_throttle_ctl
TEST_SCRIPT=${ROOT}/tests/test_cli_excluded_types.sh

echo "=== Comprehensive Test Suite ==="
echo "Running build, API tests, CTL tests, and legacy regression tests"
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

echo "Running comprehensive API tests..."
chmod +x "$ROOT/tests/test_api_complete.sh"
"$ROOT/tests/test_api_complete.sh"

echo "Running comprehensive CTL flag tests..."
chmod +x "$ROOT/tests/test_ctl_flags.sh"
"$ROOT/tests/test_ctl_flags.sh"

echo "Running legacy regression tests..."
echo "Running CLI excluded-types tests..."
chmod +x "$TEST_SCRIPT"
"$TEST_SCRIPT"

echo "Running HTTP excluded-types tests..."
chmod +x "$ROOT/tests/test_http_excluded_types.sh"
"$ROOT/tests/test_http_excluded_types.sh"

echo "Running normalization tests..."
chmod +x "$ROOT/tests/test_normalize_excluded_types.sh"
"$ROOT/tests/test_normalize_excluded_types.sh"

echo "🎉 All comprehensive tests passed!"
echo ""
echo "Test coverage:"
echo "  • Build verification"
echo "  • Complete API endpoint testing"
echo "  • Complete CTL command/flag testing"
echo "  • Legacy regression tests (excluded-types)"
echo ""
exit 0
