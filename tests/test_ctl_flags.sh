#!/usr/bin/env bash
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
BIN=${ROOT}/cpu_throttle
CTL=${ROOT}/cpu_throttle_ctl

echo "=== Complete CTL Flag Tests ==="
echo "Testing all ctl commands and flags safely"

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

echo "✅ Daemon ready"

# Test all ctl commands and flags
echo ""
echo "Testing ctl commands..."

# 1. Basic status commands
echo -n "status... "
if "$CTL" status >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

echo -n "status --json... "
if "$CTL" status --json | jq -e '.temperature' >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

echo -n "status --pretty... "
if "$CTL" status --pretty >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

# 2. Limits command
echo -n "limits... "
if "$CTL" limits >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

echo -n "limits --json... "
if "$CTL" limits --json | jq -e '.cpu_min_freq and .cpu_max_freq' >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

# 3. Zones command
echo -n "zones... "
if "$CTL" zones >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

echo -n "zones --json... "
if "$CTL" zones --json >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

# 4. Version command
echo -n "version... "
if "$CTL" version >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

# 5. Excluded types commands
echo -n "get-excluded-types... "
if "$CTL" get-excluded-types >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

echo -n "get-excluded-types --json... "
if "$CTL" get-excluded-types --json >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

echo -n "get-excluded-types --pretty... "
if "$CTL" get-excluded-types --pretty >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

# 6. Profile commands (safe operations)
echo -n "list-profiles... "
if "$CTL" list-profiles >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

echo -n "list-profiles --json... "
if "$CTL" list-profiles --json >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

# 7. Skin commands (safe operations)
echo -n "skins list... "
if "$CTL" skins list >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

echo -n "skins list --json... "
if "$CTL" skins list --json >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

echo -n "skins list --pretty... "
if "$CTL" skins list --pretty >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

# 8. Test invalid commands (should show error message)
echo -n "Testing invalid command... "
if "$CTL" invalid-command 2>&1 | grep -q "ERROR"; then
    echo "✅ (shows error)"
else
    echo "❌ (no error shown)"
    exit 1
fi

# 9. Test help
echo -n "Testing help output... "
if "$CTL" --help >/dev/null 2>&1 || "$CTL" -h >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

# 10. Test flag combinations
echo -n "Testing flag combinations... "
if "$CTL" get-excluded-types --json --pretty >/dev/null 2>&1; then
    echo "✅"
else
    echo "❌"
    exit 1
fi

echo ""
echo "🎉 All ctl flag tests passed!"
echo ""
echo "Tested commands and flags:"
echo "  • status [--json|--pretty]"
echo "  • limits [--json]"
echo "  • zones [--json]"
echo "  • version"
echo "  • get-excluded-types [--json|--pretty]"
echo "  • list-profiles [--json]"
echo "  • skins list [--json|--pretty]"
echo "  • Invalid command handling"
echo "  • Help output"
echo "  • Flag combinations"

exit 0