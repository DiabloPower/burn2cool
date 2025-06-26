#!/bin/bash

# Target paths
SCRIPT_PATH="/usr/local/bin/dynamic-tlp.sh"
SERVICE_PATH="/etc/systemd/system/dynamic-tlp.service"

echo "➡️ Installing temperature-based TLP profile with 70°C curve..."

# Step 1: Write main script
cat << 'EOF' | sudo tee $SCRIPT_PATH > /dev/null
#!/bin/bash

# Paths and values
LAST_FREQ=""
# Automatically read min/max from CPU
CPUFREQ_PATH="/sys/devices/system/cpu/cpu0/cpufreq"
MIN_FREQ=$(<"$CPUFREQ_PATH/cpuinfo_min_freq")
MAX_FREQ=$(<"$CPUFREQ_PATH/cpuinfo_max_freq")

# CPU temperature path (adjust if necessary)
TEMP_PATH="/sys/class/thermal/thermal_zone0/temp"

# Main loop
while true; do
    if [[ -f "$TEMP_PATH" ]]; then
        RAW_TEMP=$(<"$TEMP_PATH")
        TEMP=$((RAW_TEMP / 1000))
    else
        echo "Temperature path not found: $TEMP_PATH"
        exit 1
    fi

    # Clamp temperature
    if [ "$TEMP" -gt 95 ]; then
        TEMP=95
    elif [ "$TEMP" -lt 70 ]; then
        TEMP=70
    fi

    # Dynamically calculate frequency
    SCALE=$(( (95 - TEMP) ))
    DELTA=$((MAX_FREQ - MIN_FREQ))
    ADJUSTED=$((MIN_FREQ + (SCALE * DELTA / 25) ))

    MAX_TAKT=$ADJUSTED

    # Set frequency if it changed
    if [ "$MAX_TAKT" != "$LAST_FREQ" ]; then
        for CPUFREQ in /sys/devices/system/cpu/cpu[0-9]*/cpufreq/scaling_max_freq; do
            echo "$MAX_TAKT" | sudo tee "$CPUFREQ" > /dev/null
        done

        echo "$(date '+%H:%M:%S') Temp: ${TEMP}°C → Max frequency: ${MAX_TAKT} kHz"
        LAST_FREQ=$MAX_TAKT
    fi

    sleep 1
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
sudo systemctl start dynamic-tlp.service

echo "✅ Setup complete – temperature-based frequency scaling from 70 °C is active!"

