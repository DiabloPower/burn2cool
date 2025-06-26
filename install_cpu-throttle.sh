#!/bin/bash

# Target paths
BINARY_NAME="cpu-throttle"
SOURCE_FILE="cpu_throttle.c"
BUILD_PATH="/usr/local/bin/$BINARY_NAME"
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

# Optional: install build-essential (only useful on apt-based systems)
if [[ "$PKG_MANAGER" == "apt-get" ]]; then
    dpkg -s build-essential &>/dev/null || install_package build-essential
fi

# Step 1: Compile
echo "🛠️ Compiling C program..."
gcc -O2 -o "$BINARY_NAME" "$SOURCE_FILE" || {
    echo "❌ Compilation failed!"
    exit 1
}

sudo mv "$BINARY_NAME" "$BUILD_PATH"
sudo chmod +x "$BUILD_PATH"

# Step 2: Create systemd service
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
sudo systemctl enable "$BINARY_NAME.service"
sudo systemctl start "$BINARY_NAME.service"

echo "✅ Service $BINARY_NAME has been successfully installed and started!"
