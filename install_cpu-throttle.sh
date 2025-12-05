#!/bin/bash

# Target paths
BINARY_NAME="cpu-throttle"
CTL_BINARY_NAME="cpu-throttle-ctl"
SOURCE_FILE="cpu_throttle.c"
CTL_SOURCE_FILE="cpu_throttle_ctl.c"
BUILD_PATH="/usr/local/bin/$BINARY_NAME"
CTL_BUILD_PATH="/usr/local/bin/$CTL_BINARY_NAME"
SERVICE_PATH="/etc/systemd/system/$BINARY_NAME.service"

# Function: Detect package manager
detect_package_manager() {
    if command -v apt-get &>/dev/null; then
        echo "apt-get"
    elif command -v dnf &>/dev/null; then
        echo "dnf"
    elif command -v yum &>/dev/null; then
        echo "yum"
    elif command -v pacman &>/dev/null; then
        echo "pacman"
    elif command -v zypper &>/dev/null; then
        echo "zypper"
    elif command -v apk &>/dev/null; then
        echo "apk"
    else
        echo ""
    fi
}

# Function: Install package
install_package() {
    local pkg="$1"
    case "$PKG_MANAGER" in
        apt-get) sudo apt-get update && sudo apt-get install -y "$pkg" ;;
        dnf) sudo dnf install -y "$pkg" ;;
        yum) sudo yum install -y "$pkg" ;;
        pacman) sudo pacman -Sy --noconfirm "$pkg" ;;
        zypper) sudo zypper install -y "$pkg" ;;
        apk) sudo apk add --no-cache "$pkg" ;;
        *) echo "❌ No supported package manager found."; exit 1 ;;
    esac
}

# Function: Install build dependencies
install_build_deps() {
    case "$PKG_MANAGER" in
        apt-get)
            if ! dpkg -s build-essential &>/dev/null; then
                echo "📦 Installing build-essential..."
                install_package build-essential
            fi
            ;;
        dnf|yum)
            # Check for glibc-devel
            if ! rpm -q glibc-devel &>/dev/null; then
                echo "📦 Installing development tools..."
                if [[ "$PKG_MANAGER" == "dnf" ]]; then
                    sudo dnf groupinstall -y "Development Tools" || install_package glibc-devel
                else
                    sudo yum groupinstall -y "Development Tools" || install_package glibc-devel
                fi
            fi
            ;;
        pacman)
            if ! pacman -Qi base-devel &>/dev/null; then
                echo "📦 Installing base-devel..."
                install_package base-devel
            fi
            ;;
        zypper)
            if ! rpm -q glibc-devel &>/dev/null; then
                echo "📦 Installing development tools..."
                sudo zypper install -y -t pattern devel_basis || install_package glibc-devel
            fi
            ;;
        apk)
            # Alpine needs build-base for full build environment
            if ! apk info -e build-base &>/dev/null; then
                echo "📦 Installing build-base..."
                install_package build-base
            fi
            ;;
    esac
}

echo "🔍 Detecting package manager..."
PKG_MANAGER=$(detect_package_manager)

if [[ -z "$PKG_MANAGER" ]]; then
    echo "❌ No supported package manager detected."
    exit 1
else
    echo "✅ Detected package manager: $PKG_MANAGER"
fi

# Step 0: Check for dependencies
if ! command -v gcc &>/dev/null; then
    echo "🧱 GCC not found – attempting installation..."
    install_package gcc
fi

# Install build dependencies (build-essential, glibc-devel, base-devel, etc.)
echo "🔧 Checking build dependencies..."
install_build_deps

# Step 0.5: Check if service already exists
SERVICE_EXISTS=false
if systemctl list-unit-files | grep -q "^$BINARY_NAME.service"; then
    SERVICE_EXISTS=true
    echo "⚠️  Service $BINARY_NAME already exists!"
    
    # Stop the running service
    if systemctl is-active --quiet "$BINARY_NAME.service"; then
        echo "🛑 Stopping running service..."
        sudo systemctl stop "$BINARY_NAME.service"
    fi
    
    # Backup old service file
    BACKUP_PATH="${SERVICE_PATH}.backup.$(date +%Y%m%d_%H%M%S)"
    if [[ -f "$SERVICE_PATH" ]]; then
        echo "💾 Backing up old service file to: $BACKUP_PATH"
        sudo cp "$SERVICE_PATH" "$BACKUP_PATH"
    fi
fi

# Step 1: Compile
echo "🛠️ Compiling C programs..."

# Compile main daemon
gcc -O2 -o "$BINARY_NAME" "$SOURCE_FILE" || {
    echo "❌ Compilation of $SOURCE_FILE failed!"
    exit 1
}

# Compile control utility
gcc -O2 -o "$CTL_BINARY_NAME" "$CTL_SOURCE_FILE" || {
    echo "❌ Compilation of $CTL_SOURCE_FILE failed!"
    exit 1
}

# Install binaries
sudo mv "$BINARY_NAME" "$BUILD_PATH"
sudo mv "$CTL_BINARY_NAME" "$CTL_BUILD_PATH"
sudo chmod +x "$BUILD_PATH"
sudo chmod +x "$CTL_BUILD_PATH"

echo "✅ Binaries installed:"
echo "   - $BUILD_PATH (daemon)"
echo "   - $CTL_BUILD_PATH (control utility)"

# Step 2: Create systemd service
# Ask user whether to enable web interface by default (writes /etc/cpu_throttle.conf)
read -r -p "Enable web interface on port 8086 by default? [Y/n]: " ENABLE_WEB
ENABLE_WEB=${ENABLE_WEB:-Y}
if [[ "$ENABLE_WEB" =~ ^[Yy] ]]; then
    echo "➡️ Enabling web interface by default (web_port=8086)"
    # Ensure config directory and file, write or replace web_port value
    sudo mkdir -p /etc
    if sudo grep -q "^web_port=" /etc/cpu_throttle.conf 2>/dev/null; then
        sudo sed -i "s/^web_port=.*/web_port=8086/" /etc/cpu_throttle.conf
    else
        echo "web_port=8086" | sudo tee -a /etc/cpu_throttle.conf > /dev/null
    fi
else
    echo "➡️ Web interface will be disabled by default. You can enable later by editing /etc/cpu_throttle.conf or using --web-port"
fi

# Firewall integration: ask to open port 8086 for common firewalls (ufw, firewalld)
WEB_PORT=8086
if [[ "$ENABLE_WEB" =~ ^[Yy] ]]; then
    # Detect ufw
    if command -v ufw &>/dev/null; then
        read -r -p "Detected ufw firewall. Open port ${WEB_PORT}/tcp? [Y/n]: " UFW_ANSWER
        UFW_ANSWER=${UFW_ANSWER:-Y}
        if [[ "$UFW_ANSWER" =~ ^[Yy] ]]; then
            echo "➡️ Adding ufw rule for port ${WEB_PORT}/tcp"
            sudo ufw allow ${WEB_PORT}/tcp
        fi
    fi

    # Detect firewalld
    if command -v firewall-cmd &>/dev/null; then
        read -r -p "Detected firewalld. Open port ${WEB_PORT}/tcp (permanent)? [Y/n]: " FWD_ANSWER
        FWD_ANSWER=${FWD_ANSWER:-Y}
        if [[ "$FWD_ANSWER" =~ ^[Yy] ]]; then
            echo "➡️ Adding firewalld rule for port ${WEB_PORT}/tcp"
            sudo firewall-cmd --permanent --add-port=${WEB_PORT}/tcp
            sudo firewall-cmd --reload
        fi
    fi

    # If neither firewall detected, print hint
    if ! command -v ufw &>/dev/null && ! command -v firewall-cmd &>/dev/null; then
        echo "ℹ️ No ufw or firewalld detected. If you run a different firewall, please open port ${WEB_PORT}/tcp manually."
    fi
fi

cat << EOF | sudo tee "$SERVICE_PATH" > /dev/null
[Unit]
Description=Temperature-based CPU frequency scaling daemon
After=multi-user.target

[Service]
Type=simple
ExecStart=$BUILD_PATH
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

# Step 3: Enable and start service
sudo systemctl daemon-reexec

if [[ "$SERVICE_EXISTS" == true ]]; then
    echo "🔄 Restarting existing service..."
    sudo systemctl enable "$BINARY_NAME.service"
    sudo systemctl restart "$BINARY_NAME.service"
else
    echo "🚀 Enabling and starting new service..."
    sudo systemctl enable "$BINARY_NAME.service"
    sudo systemctl start "$BINARY_NAME.service"
fi

# Wait a moment and check if service started successfully
sleep 1
if systemctl is-active --quiet "$BINARY_NAME.service"; then
    echo ""
    echo "✅ Service $BINARY_NAME has been successfully installed and started!"
else
    echo ""
    echo "⚠️  Service installed but may have failed to start. Check logs:"
    echo "   sudo journalctl -u $BINARY_NAME -n 20"
fi

echo ""
echo "📋 Usage:"
echo "   - Control daemon: $CTL_BINARY_NAME <command>"
echo "   - Show status:    $CTL_BINARY_NAME status"
echo "   - Set max freq:   $CTL_BINARY_NAME set-safe-max 4000000"
echo "   - Set min freq:   $CTL_BINARY_NAME set-safe-min 2000000"
echo "   - Set max temp:   $CTL_BINARY_NAME set-temp-max 85"
echo "   - Quit daemon:    $CTL_BINARY_NAME quit"
echo ""
echo "📊 Check service status: sudo systemctl status $BINARY_NAME"
echo "📜 View logs:           sudo journalctl -u $BINARY_NAME -f"
