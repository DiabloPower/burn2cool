#!/bin/bash

# Target paths
SCRIPT_PATH="/usr/local/bin/dynamic-tlp.sh"
SERVICE_PATH="/etc/systemd/system/dynamic-tlp.service"

echo "➡️ Installing modernized temperature-based CPU frequency control..."

# Step 1: Write main script
cat << 'EOF' | sudo tee $SCRIPT_PATH > /dev/null
#!/bin/bash

# Configuration
TEMP_MAX=95        # Maximum temperature in °C
SAFE_MIN=0         # Minimum frequency limit (0 = use CPU min)
SAFE_MAX=0         # Maximum frequency limit (0 = use CPU max)
CHECK_INTERVAL=1   # Seconds between checks

# Automatically read min/max from CPU
CPUFREQ_PATH="/sys/devices/system/cpu/cpu0/cpufreq"
CPU_MIN_FREQ=$(<"$CPUFREQ_PATH/cpuinfo_min_freq")
CPU_MAX_FREQ=$(<"$CPUFREQ_PATH/cpuinfo_max_freq")

# Apply user limits or use CPU defaults
MIN_FREQ=${SAFE_MIN:-$CPU_MIN_FREQ}
MAX_FREQ=${SAFE_MAX:-$CPU_MAX_FREQ}
[ "$MIN_FREQ" -eq 0 ] && MIN_FREQ=$CPU_MIN_FREQ
[ "$MAX_FREQ" -eq 0 ] && MAX_FREQ=$CPU_MAX_FREQ

# CPU temperature path (adjust if necessary)
TEMP_PATH="/sys/class/thermal/thermal_zone0/temp"

# Verify temperature sensor
if [[ ! -f "$TEMP_PATH" ]]; then
    echo "ERROR: Temperature sensor not found: $TEMP_PATH"
    exit 1
fi

echo "CPU Frequency Range: $(($CPU_MIN_FREQ / 1000)) - $(($CPU_MAX_FREQ / 1000)) MHz"
echo "Active Limits: $(($MIN_FREQ / 1000)) - $(($MAX_FREQ / 1000)) MHz"
echo "Temperature Maximum: ${TEMP_MAX}°C"
echo "Starting temperature-based frequency control..."

# Calculate temperature thresholds (proportional to temp_max)
THRESH_LIGHT=$(( TEMP_MAX * 79 / 100 ))   # 75°C at 95°C max
THRESH_MEDIUM=$(( TEMP_MAX * 86 / 100 ))  # 82°C at 95°C max
THRESH_STRONG=$(( TEMP_MAX * 93 / 100 ))  # 88°C at 95°C max

LAST_FREQ=""
FREQ_RANGE=$(( MAX_FREQ - MIN_FREQ ))
HYSTERESIS=$(( FREQ_RANGE / 10 ))  # 10% hysteresis

# Main loop
while true; do
    RAW_TEMP=$(<"$TEMP_PATH")
    TEMP=$(( RAW_TEMP / 1000 ))

    # Calculate target frequency based on temperature
    if [ "$TEMP" -lt "$THRESH_LIGHT" ]; then
        # Cool: 100% performance
        TARGET_FREQ=$MAX_FREQ
    elif [ "$TEMP" -lt "$THRESH_MEDIUM" ]; then
        # Light throttling: 85%
        TARGET_FREQ=$(( MIN_FREQ + (FREQ_RANGE * 85 / 100) ))
    elif [ "$TEMP" -lt "$THRESH_STRONG" ]; then
        # Medium throttling: 65%
        TARGET_FREQ=$(( MIN_FREQ + (FREQ_RANGE * 65 / 100) ))
    elif [ "$TEMP" -lt "$TEMP_MAX" ]; then
        # Strong throttling: 40%
        TARGET_FREQ=$(( MIN_FREQ + (FREQ_RANGE * 40 / 100) ))
    else
        # Emergency: minimum frequency
        TARGET_FREQ=$MIN_FREQ
    fi

    # Apply hysteresis to prevent rapid switching
    if [ -n "$LAST_FREQ" ]; then
        FREQ_DIFF=$(( TARGET_FREQ > LAST_FREQ ? TARGET_FREQ - LAST_FREQ : LAST_FREQ - TARGET_FREQ ))
        if [ "$FREQ_DIFF" -lt "$HYSTERESIS" ]; then
            sleep "$CHECK_INTERVAL"
            continue
        fi
    fi

    # Set frequency if it changed significantly
    if [ "$TARGET_FREQ" != "$LAST_FREQ" ]; then
        for CPUFREQ in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_max_freq; do
            echo "$TARGET_FREQ" | sudo tee "$CPUFREQ" > /dev/null 2>&1
        done

        echo "$(date '+%H:%M:%S') Temp: ${TEMP}°C → Max frequency: $(($TARGET_FREQ / 1000)) MHz"
        LAST_FREQ=$TARGET_FREQ
    fi

    sleep "$CHECK_INTERVAL"
done
EOF

sudo chmod +x $SCRIPT_PATH

# Step 2: Create systemd service
cat << EOF | sudo tee $SERVICE_PATH > /dev/null
[Unit]
Description=Dynamic CPU frequency throttling via temperature (TLP)
After=multi-user.target

[Service]
Type=simple
ExecStart=$SCRIPT_PATH
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

# Step 3: Enable & start service
sudo systemctl daemon-reexec
sudo systemctl enable dynamic-tlp.service
sudo systemctl restart dynamic-tlp.service

echo ""
echo "✅ Setup complete!"
echo ""
echo "📋 Service Management:"
echo "   sudo systemctl status dynamic-tlp.service   # Check status"
echo "   sudo systemctl stop dynamic-tlp.service     # Stop service"
echo "   sudo journalctl -u dynamic-tlp.service -f   # View logs"
echo ""
echo "⚙️  Configuration (edit $SCRIPT_PATH):"
echo "   TEMP_MAX=95        # Maximum temperature threshold"
echo "   SAFE_MIN=0         # Minimum frequency limit (0 = auto)"
echo "   SAFE_MAX=0         # Maximum frequency limit (0 = auto)"
echo "   CHECK_INTERVAL=1   # Seconds between checks"
echo ""
echo "💡 For advanced features (runtime control, profiles), use cpu_throttle instead!"

