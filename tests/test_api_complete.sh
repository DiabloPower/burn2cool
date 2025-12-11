#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${ROOT}/cpu_throttle
CTL=${ROOT}/cpu_throttle_ctl
API_URL=http://127.0.0.1:8086

echo "=== Complete API Tests ==="
echo "Testing all REST API endpoints safely"

cd "$ROOT"

# Check if daemon is already running
DAEMON_RUNNING=0
if [ -S /tmp/cpu_throttle.sock ]; then
    echo "⚠️  Daemon socket exists - testing against existing daemon"
    DAEMON_RUNNING=1
else
    echo "Starting test daemon..."
    "$BIN" --web-port --quiet &
    PID=$!
    DAEMON_RUNNING=0
    echo "daemon pid=$PID"
    trap 'echo "Stopping test daemon..."; kill $PID || true; wait $PID 2>/dev/null || true' EXIT
fi

# Wait for daemon to be ready
echo "Waiting for daemon..."
timeout=20
while [[ $timeout -gt 0 ]]; do
    if [ -S /tmp/cpu_throttle.sock ]; then
        break
    fi
    sleep 1
    timeout=$((timeout-1))
done

if [[ $timeout -eq 0 ]]; then
    echo "❌ Daemon didn't start within 20 seconds"
    exit 1
fi

# Wait for HTTP API
echo "Waiting for HTTP API..."
timeout=10
while [[ $timeout -gt 0 ]]; do
    if curl -sSf "$API_URL/api/status" >/dev/null 2>&1; then
        break
    fi
    sleep 1
    timeout=$((timeout-1))
done

if [[ $timeout -eq 0 ]]; then
    echo "❌ HTTP API not available within 10 seconds"
    exit 1
fi

echo "✅ Daemon and API ready"

# Test all API endpoints
echo ""
echo "Testing API endpoints..."

# 1. Status endpoint
echo -n "GET /api/status... "
if curl -sS "$API_URL/api/status" | jq -e '.temperature and .frequency' >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

# 2. Limits endpoint
echo -n "GET /api/limits... "
if curl -sS "$API_URL/api/limits" | jq -e '.cpu_min_freq and .cpu_max_freq' >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

# 3. Zones endpoint
echo -n "GET /api/zones... "
if curl -sS "$API_URL/api/zones" | jq -e '.zones and (.zones | type == "array")' >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

# 4. Metrics endpoint
echo -n "GET /api/metrics... "
if curl -sS "$API_URL/api/metrics" | jq -e '.cpu_frequencies' >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

# 5. Settings endpoints (read-only)
echo -n "GET /api/settings/excluded-types... "
if curl -sS "$API_URL/api/settings/excluded-types" >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

# 6. Test error handling for POST (should fail safely)
echo -n "Testing POST error handling... "
if curl -sS -X POST -H 'Content-Type: application/json' -d '{}' "$API_URL/api/invalid-endpoint" | jq -e '.status == "error"' >/dev/null 2>&1; then
    echo "✅ (proper error response)"
else
    echo "❌ (no proper error)"
    exit 1
fi

# 7. Profiles endpoints
echo -n "GET /api/profiles... "
if curl -sS "$API_URL/api/profiles" | jq -e '.profiles and (.profiles | type == "array")' >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

# 8. Skins endpoints
echo -n "GET /api/skins... "
if curl -sS "$API_URL/api/skins" | jq -e '.skins and (.skins | type == "array")' >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

# 9. Test error handling
echo -n "Testing invalid endpoint... "
if curl -sS "$API_URL/api/invalid" | jq -e '.status == "error"' >/dev/null 2>&1; then
    echo "✅ (returns error JSON)"
else
    echo "❌ (no proper error response)"
    exit 1
fi

echo ""
echo "🎉 All API tests passed!"
echo ""
echo "Tested endpoints:"
echo "  • GET  /api/status"
echo "  • GET  /api/limits"
echo "  • GET  /api/zones"
echo "  • GET  /api/metrics"
echo "  • GET  /api/settings/excluded-types"
echo "  • POST error handling"
echo "  • GET  /api/profiles"
echo "  • GET  /api/skins"
echo "  • Error handling"

exit 0