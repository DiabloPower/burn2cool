#!/usr/bin/env bash
set -euo pipefail

# Consolidated installer v2 for cpu_throttle
# - Based on user's draft and existing `install_cpu-throttle.sh`
# - Preserves provided variable names (including DEAMON_* typos)

# =========================
# Config and naming
# =========================

BINARY_NAME="cpu_throttle"
CTL_BINARY_NAME="cpu_throttle_ctl"
TUI_BIN_NAME="cpu_throttle_tui"

# Maintain provided variable names (typos included) for compatibility
DEAMON_NAME="$BINARY_NAME"
DEAMON_CTL_NAME="$CTL_BINARY_NAME"
TUI_BUILD_NAME="$TUI_BIN_NAME"

DEAMON_PATH="/usr/local/bin/$BINARY_NAME"
DEAMON_CTL_PATH="/usr/local/bin/$CTL_BINARY_NAME"
TUI_BUILD_PATH="/usr/local/bin/$TUI_BIN_NAME"

SERVICE_PATH="/etc/systemd/system/$BINARY_NAME.service"
BUILD_PATH="$DEAMON_PATH" # For compatibility with provided service template

REPO_OWNER="DiabloPower"
REPO_NAME="burn2cool"
RELEASES_URL="https://github.com/${REPO_OWNER}/${REPO_NAME}/releases"
API_LATEST="https://api.github.com/repos/${REPO_OWNER}/${REPO_NAME}/releases/latest"
API_TAG_BASE="https://api.github.com/repos/${REPO_OWNER}/${REPO_NAME}/releases/tags"

DEFAULT_PORTS=(8086 8286 8386 8486)

# =========================
# State and flags
# =========================

YES=0
QUIET=0
DEBUG_INSTALL=0
FORCE_BUILD=0
FORCE_BINARIES=0
ENABLE_TUI=0
INSTALL_TUI_PREF=0
RELEASE_TAG=""
FETCH_URL=""
WEB_ENABLE=0
WEB_PORT=""

# Compatibility with other installer naming
AUTO_YES=0

# =========================
# Logging helpers
# =========================

log() { [ "${QUIET:-0}" -eq 0 ] && echo -e "$*"; }
warn() { echo -e "WARNING: $*" >&2; }
err() { echo -e "ERROR: $*" >&2; }

# =========================
# Prompt helper (respects --yes)
# =========================

prompt_yes_no() {
  local msg="$1"
  if [ "${YES:-0}" -eq 1 ] || [ "${AUTO_YES:-0}" -eq 1 ]; then
    log "$msg [auto: yes]"
    return 0
  fi
  read -r -p "$msg [y/N]: " ans
  case "${ans,,}" in
    y|yes) return 0 ;;
    *)     return 1 ;;
  esac
}

prompt_input() {
  local msg="$1" default="${2:-}"
  if [ "${YES:-0}" -eq 1 ] && [ -n "$default" ]; then
    echo "$default"
    return 0
  fi
  read -r -p "$msg${default:+ [$default]}: " ans
  if [ -z "$ans" ] && [ -n "$default" ]; then echo "$default"; else echo "$ans"; fi
}

# =========================
# Arg parsing
# =========================

while (( "$#" )); do
  case "$1" in
    -y|--yes|--non-interactive) YES=1; AUTO_YES=1 ;;
    --tui|--enable-tui) ENABLE_TUI=1 ;;
    --install-tui) INSTALL_TUI_PREF=1 ;;
    --force-build) FORCE_BUILD=1 ;;
    --force-binaries) FORCE_BINARIES=1 ;;
    --release-tag) RELEASE_TAG="${2:-}"; shift ;;
    --fetch-url) FETCH_URL="${2:-}"; shift ;;
    --quiet) QUIET=1 ;;
    --debug-install) DEBUG_INSTALL=1 ;;
    --web-port) WEB_ENABLE=1; WEB_PORT="${2:-}"; shift ;;
    --help|-h)
      cat <<USAGE
Usage: $(basename "$0") [options]
  -y, --yes, --non-interactive     Accept defaults, no prompts
  --tui, --enable-tui              Build/install TUI from source
  --install-tui                    Prefer installing prebuilt TUI if present
  --force-build                    Force build from source
  --force-binaries                 Force install of prebuilt binaries
  --release-tag <tag>              Pin specific release tag
  --fetch-url <url>                Explicit artifact URL (zip/tar/binary)
  --quiet                          Reduce output
  --debug-install                  Save debug artifacts to /tmp
  --web-port <port>                Enable web API with a specific port
USAGE
      exit 0
      ;;
    *)
      warn "Unknown option: $1"
      ;;
  esac
  shift
done

# =========================
# Greeting
# =========================

log "==============================================================="
log " Burn2Cool installer for ${BINARY_NAME} (temperature-based CPU governor)"
log " Repo: https://github.com/${REPO_OWNER}/${REPO_NAME}"
log " This will:"
log "  - Download binaries or build from source"
log "  - Stage in a temp dir for consistent processing"
log "  - Stop any running daemon and install new binaries"
log "  - Optionally enable web API and configure firewall"
log "  - Create/refresh the systemd service and start it"
log "==============================================================="

# =========================
# Detect package manager
# =========================

PKG=""
detect_pkg() {
  if command -v apt-get >/dev/null 2>&1; then PKG="apt-get"
  elif command -v dnf >/dev/null 2>&1; then PKG="dnf"
  elif command -v yum >/dev/null 2>&1; then PKG="yum"
  elif command -v zypper >/dev/null 2>&1; then PKG="zypper"
  elif command -v apk >/dev/null 2>&1; then PKG="apk"
  elif command -v pacman >/dev/null 2>&1; then PKG="pacman"
  else
    PKG=""
  fi
}
detect_pkg

if [ -z "$PKG" ]; then
  err "No supported package manager found (apt-get, dnf, yum, zypper, apk, pacman)."
  # Not fatal: continue but warn
  warn "Installer may not be able to auto-install dependencies."
fi

# =========================
# Install dependencies
# =========================

deps_common=(curl unzip tar jq make gcc)
deps_systemd=(systemd)

install_deps() {
  # Install packages mapping the generic names to distro-specific package names
  local pkgs=("$@")
  case "$PKG" in
    apt-get)
      sudo apt-get update -y
      sudo apt-get install -y "${pkgs[@]}"
      ;;
    dnf)
      sudo dnf install -y "${pkgs[@]}"
      ;;
    yum)
      sudo yum install -y "${pkgs[@]}"
      ;;
    zypper)
      sudo zypper refresh || true
      sudo zypper install -y "${pkgs[@]}"
      ;;
    apk)
      sudo apk add --no-cache "${pkgs[@]}"
      ;;
    pacman)
      sudo pacman -Sy --noconfirm "${pkgs[@]}"
      ;;
    *)
      warn "No package manager available to install: ${pkgs[*]}"
      ;;
  esac
}

# Attempt to regenerate asset headers (uses xxd). If unavailable, return non-zero
regenerate_asset_headers() {
  local assets_dir="assets"
  local include_dir="include"
  mkdir -p "$include_dir"
  if command -v xxd >/dev/null 2>&1; then
    log "Generating asset headers with xxd"
    xxd -i "$assets_dir/index.html" > "$include_dir/index_html.h" || return 1
    xxd -i "$assets_dir/main.js" > "$include_dir/main_js.h" || return 1
    xxd -i "$assets_dir/styles.css" > "$include_dir/styles_css.h" || return 1
    xxd -i "$assets_dir/favicon.ico" > "$include_dir/favicon_ico.h" || return 1
  else
    warn "xxd not available; cannot regenerate asset headers automatically. Keeping existing headers if present."
    return 1
  fi
  cat > "$include_dir/assets_generated.h" <<'H'
#define USE_ASSET_HEADERS 1
H
  return 0
}

# Do not pre-install general dependencies here; dependency installation
# for building from source happens only in the 'source' branch.

# Some minimal systems may not have systemd; try to install if manager supports it
if ! command -v systemctl >/dev/null 2>&1; then
  if [ -n "$PKG" ]; then
    log "Attempting systemd installation (if available on your distro)..."
    install_deps "${deps_systemd[@]}" || warn "Systemd not installed; service setup may be skipped."
  else
    warn "systemctl not found and no package manager detected; skipping systemd installation."
  fi
fi

# =========================
# Working directory and cleanup
# =========================

WORK_DIR="$(mktemp -d /tmp/burn2cool.XXXXXX)"
trap 'if [ "${DEBUG_INSTALL:-0}" -eq 0 ]; then rm -rf "$WORK_DIR"; else log "Debug artifacts kept in $WORK_DIR"; fi' EXIT
log "Using work directory: $WORK_DIR"

# Optional separate debug stash
DEBUG_DIR="/tmp/burn2cool-debug"
[ "${DEBUG_INSTALL:-0}" -eq 1 ] && mkdir -p "$DEBUG_DIR"

# =========================
# Asset selection logic
# =========================

choose_mode() {
  if [ -n "$FETCH_URL" ]; then echo "fetch"; return; fi
  if [ "$FORCE_BUILD" -eq 1 ] && [ "$FORCE_BINARIES" -eq 1 ]; then
    warn "Both --force-build and --force-binaries set; defaulting to binaries."
    echo "binaries"; return
  fi
  if [ "$FORCE_BUILD" -eq 1 ]; then echo "source"; return; fi
  if [ "$FORCE_BINARIES" -eq 1 ]; then echo "binaries"; return; fi
  if [ "$YES" -eq 1 ] || [ "$AUTO_YES" -eq 1 ]; then echo "binaries"; return; fi

  # Hinweise hier rausnehmen
  local choice
  choice="$(prompt_input $'Select download mode:\n  1) Prebuilt binaries (from releases/latest)\n  2) Build from source (main.zip + make)\nEnter 1 or 2' '1')"
  case "$choice" in
    1) echo "binaries" ;;
    2) echo "source" ;;
    *) echo "binaries" ;;
  esac
}

# =========================
# Download helpers
# =========================

download_url() {
  local url="$1" out="$2"
  log "Fetching: $url"
  curl -fL --progress-bar "$url" -o "$out"
}

# Wrapper for GitHub API requests to ensure a User-Agent and v3 accept header
curl_api() {
  local url="$1"
  curl -fsSL -H "Accept: application/vnd.github.v3+json" -A "burn2cool-installer" "$url"
}

filter_assets_and_download() {
  local api="$API_LATEST"
  if [ -n "$RELEASE_TAG" ]; then api="${API_TAG_BASE}/${RELEASE_TAG}"; fi
  log "Querying release assets..."
  local json
  json="$(curl_api "$api")"

  # Extract asset names and URLs using jq if available; fallback to crude parsing
  local names urls
  if command -v jq >/dev/null 2>&1; then
    mapfile -t names < <(echo "$json" | jq -r '.assets[].name')
    mapfile -t urls < <(echo "$json" | jq -r '.assets[].browser_download_url')
  else
    # naive fallback
    mapfile -t urls < <(echo "$json" | sed -n 's/.*"browser_download_url": *"\([^"\]*\)".*/\1/p')
    mapfile -t names < <(for u in "${urls[@]}"; do basename "$u"; done)
  fi

  local selected=0
  for i in "${!names[@]}"; do
    n="${names[$i]}"; u="${urls[$i]}"
    if [[ "$n" =~ \.sh$ ]] || [[ "$n" =~ autobuild ]] || [[ "$n" =~ \.(zip|tar\.gz|tgz)$ ]]; then
      continue
    fi
    if [[ "$n" == *"$BINARY_NAME"* ]] || [[ "$n" == *"$CTL_BINARY_NAME"* ]] || [[ "$n" == *"$TUI_BIN_NAME"* ]]; then
      download_url "$u" "$WORK_DIR/$n"
      chmod +x "$WORK_DIR/$n" || true
      selected=$((selected+1))
    fi
  done

  if [ "$selected" -eq 0 ]; then
    warn "No direct binary assets matched expected names; attempting fallback by excluding archives."
    for i in "${!names[@]}"; do
      n="${names[$i]}"; u="${urls[$i]}"
      if [[ ! "$n" =~ \.(zip|tar\.gz|tgz|sh)$ ]]; then
        download_url "$u" "$WORK_DIR/$n"
        chmod +x "$WORK_DIR/$n" || true
        selected=$((selected+1))
      fi
    done
  fi

  if [ "$selected" -eq 0 ]; then
    err "No suitable binary assets found in the release."
    return 1
  fi
}

# Check for required deps and install missing ones via detected package manager
check_and_install_deps() {
  local needed=()
  local deps=(curl unzip tar jq make gcc git pkg-config xxd)

  for dep in "${deps[@]}"; do
    if ! command -v "$dep" >/dev/null 2>&1; then
      needed+=("$dep")
    fi
  done

  # Ensure ncurses is present (headers/libs) — required for TUI and build
  if ! has_ncurses; then
    needed+=("ncurses")
  fi

  if [ "${#needed[@]}" -eq 0 ]; then
    log "All build dependencies already satisfied."
    return
  fi

  log "Missing dependencies: ${needed[*]}"

  # Build a distro-specific package list from the generic 'needed' items
  local pkgs=()
  case "$PKG" in
    apt-get)
      # provide meta packages useful for building
      pkgs+=(build-essential)
      for i in "${needed[@]}"; do
        case "$i" in
          ncurses) pkgs+=(libncurses-dev) ;;
          pkg-config) pkgs+=(pkg-config) ;;
          xxd) pkgs+=(xxd) ;;
          *) pkgs+=("$i") ;;
        esac
      done
      ;;
    dnf)
      pkgs+=(gcc make)
      for i in "${needed[@]}"; do
        case "$i" in
          ncurses) pkgs+=(ncurses-devel) ;;
          pkg-config) pkgs+=(pkgconf-pkg-config) ;;
          xxd) pkgs+=(vim-common) ;;
          *) pkgs+=("$i") ;;
        esac
      done
      ;;
    yum)
      pkgs+=(gcc make)
      for i in "${needed[@]}"; do
        case "$i" in
          ncurses) pkgs+=(ncurses-devel) ;;
          pkg-config) pkgs+=(pkgconf-pkg-config) ;;
          xxd) pkgs+=(vim-common) ;;
          *) pkgs+=("$i") ;;
        esac
      done
      ;;
    zypper)
      for i in "${needed[@]}"; do
        case "$i" in
          ncurses) pkgs+=(ncurses-devel) ;;
          pkg-config) pkgs+=(pkg-config) ;;
          xxd) pkgs+=(xxd) ;;
          *) pkgs+=("$i") ;;
        esac
      done
      ;;
    apk)
      pkgs+=(build-base)
      for i in "${needed[@]}"; do
        case "$i" in
          ncurses) pkgs+=(ncurses-dev) ;;
          pkg-config) pkgs+=(pkgconfig) ;;
          xxd) pkgs+=(vim) ;;
          *) pkgs+=("$i") ;;
        esac
      done
      ;;
    pacman)
      pkgs+=(base-devel)
      for i in "${needed[@]}"; do
        case "$i" in
          ncurses) pkgs+=(ncurses) ;;
          pkg-config) pkgs+=(pkgconf) ;;
          xxd) pkgs+=(xxd) ;;
          *) pkgs+=("$i") ;;
        esac
      done
      ;;
    *)
      warn "No package manager available to install: ${needed[*]}"
      ;;
  esac

  if [ "${#pkgs[@]}" -gt 0 ]; then
    install_deps "${pkgs[@]}"
  else
    warn "No distro-specific packages resolved for: ${needed[*]}"
  fi
}

# Check whether ncurses headers/libraries are already available
has_ncurses() {
  # Check via pkg-config for ncurses or ncursesw
  if command -v pkg-config >/dev/null 2>&1; then
    if pkg-config --exists ncursesw 2>/dev/null || pkg-config --exists ncurses 2>/dev/null; then
      return 0
    fi
  fi
  # Try to compile a tiny program including possible headers
  if command -v gcc >/dev/null 2>&1; then
    if printf '%s\n' '#include <ncurses.h>' 'int main(void){return 0;}' | gcc -x c - -o /dev/null - >/dev/null 2>&1; then
      return 0
    fi
    if printf '%s\n' '#include <ncursesw/ncurses.h>' 'int main(void){return 0;}' | gcc -x c - -o /dev/null - >/dev/null 2>&1; then
      return 0
    fi
  fi
  return 1
}
download_source_and_build() {
  # Prefer tarball archive from the repo (matches original installer behavior)
  local archive_url
  if [ -n "$RELEASE_TAG" ]; then
    archive_url="https://github.com/${REPO_OWNER}/${REPO_NAME}/archive/refs/tags/${RELEASE_TAG}.tar.gz"
  else
    archive_url="https://github.com/${REPO_OWNER}/${REPO_NAME}/archive/refs/heads/main.tar.gz"
  fi
  download_url "$archive_url" "$WORK_DIR/main.tar.gz"
  (cd "$WORK_DIR" && tar -xzf main.tar.gz)
  local src_dir
  src_dir="$(find "$WORK_DIR" -maxdepth 3 -type d -name "${REPO_NAME}-*" | grep -E "${REPO_NAME}-(main|${RELEASE_TAG:-})" | head -n1 || true)"
  # fallback: any directory that looks like the repo
  if [ -z "$src_dir" ]; then
    src_dir="$(find "$WORK_DIR" -maxdepth 2 -type d -name "${REPO_NAME}-*" | head -n1 || true)"
  fi
  [ -z "$src_dir" ] && err "Source directory not found after unzip." && exit 1

  log "Building from source with make..."
  # Attempt to regenerate asset headers in source tree; if successful,
  # remove pre-generated headers so make assets will recreate them.
  if (cd "$src_dir" && regenerate_asset_headers); then
    log "Regenerated asset headers in source tree"
  else
    log "Using existing asset headers if present"
  fi

  if [ -d "$src_dir" ]; then
    if command -v bash >/dev/null 2>&1; then
      MAKE_SHELL_ARG="SHELL=/bin/bash"
    else
      MAKE_SHELL_ARG=""
    fi
    (cd "$src_dir" && if make $MAKE_SHELL_ARG assets 2>/dev/null || true; then true; fi)
    (cd "$src_dir" && make $MAKE_SHELL_ARG -j"$(nproc)" )
  else
    (cd "$src_dir" && make)
  fi

  # Collect built binaries (search common build paths)
  local built=0
  for candidate in \
    "$src_dir/$BINARY_NAME" \
    "$src_dir/bin/$BINARY_NAME" \
    "$src_dir/build/$BINARY_NAME" \
    "$src_dir/out/$BINARY_NAME"
  do
    if [ -f "$candidate" ]; then
      install -m 0755 "$candidate" "$WORK_DIR/$BINARY_NAME"
      built=$((built+1))
      break
    fi
  done

  for candidate in \
    "$src_dir/$CTL_BINARY_NAME" \
    "$src_dir/bin/$CTL_BINARY_NAME" \
    "$src_dir/build/$CTL_BINARY_NAME" \
    "$src_dir/out/$CTL_BINARY_NAME"
  do
    if [ -f "$candidate" ]; then
      install -m 0755 "$candidate" "$WORK_DIR/$CTL_BINARY_NAME"
      built=$((built+1))
      break
    fi
  done

  if [ "$ENABLE_TUI" -eq 1 ]; then
    for candidate in \
      "$src_dir/$TUI_BIN_NAME" \
      "$src_dir/bin/$TUI_BIN_NAME" \
      "$src_dir/build/$TUI_BIN_NAME" \
      "$src_dir/out/$TUI_BIN_NAME"
    do
      if [ -f "$candidate" ]; then
        install -m 0755 "$candidate" "$WORK_DIR/$TUI_BIN_NAME"
        built=$((built+1))
        break
      fi
    done
  fi

  if [ "$built" -eq 0 ]; then
    err "No suitable binary assets found in the release."
    return 1
  fi

  if [ "$DEBUG_INSTALL" -eq 1 ]; then
    cp -a "$WORK_DIR"/* "$DEBUG_DIR/" || true
    (cd "$src_dir" && [ -d include ] && tar -c --xz -f "$DEBUG_DIR/include.txz" include) || true
    (cd "$WORK_DIR" && sha256sum * > "$DEBUG_DIR/sha256.sum") || true
  fi
}

download_explicit_url() {
  local ext
  ext="${FETCH_URL##*.}"
  case "$ext" in
    zip)
      download_url "$FETCH_URL" "$WORK_DIR/custom.zip"
      (cd "$WORK_DIR" && unzip -q custom.zip)
      ;;
    tar|gz|tgz)
      download_url "$FETCH_URL" "$WORK_DIR/custom.tar.gz"
      (cd "$WORK_DIR" && tar -xzf custom.tar.gz)
      ;;
    *)
      # Treat as raw binary
      local fname
      fname="$(basename "$FETCH_URL")"
      download_url "$FETCH_URL" "$WORK_DIR/$fname"
      chmod +x "$WORK_DIR/$fname" || true
      ;;
  esac
}

# =========================
# Perform download/build
# =========================

mode="$(choose_mode)"
case "$mode" in
  binaries) filter_assets_and_download ;;
  source)
    log "Checking build dependencies..."
    check_and_install_deps
    download_source_and_build
    ;;
  fetch) download_explicit_url ;;
esac

# =========================
# Stage verification
# =========================

log "Staged files in $WORK_DIR:"
ls -1 "$WORK_DIR" || true

# =========================
# Stop running daemon if present
# =========================

stop_daemon() {
  # Hardened stop: temporarily disable errexit so any unexpected non-zero
  # return values in this function do not kill the entire installer.
  set +e
  local stopped=0

  if command -v systemctl >/dev/null 2>&1; then
    # If the service is active, stop it (ignore failures)
    if systemctl is-active --quiet "$BINARY_NAME" 2>/dev/null; then
      log "Stopping systemd service: $BINARY_NAME"
      sudo systemctl stop "$BINARY_NAME" >/dev/null 2>&1 || warn "Failed to stop service $BINARY_NAME"
      # wait briefly and confirm it stopped; if not, escalate
      local i=0
      while systemctl is-active --quiet "$BINARY_NAME" 2>/dev/null && [ $i -lt 5 ]; do
        sleep 1
        i=$((i+1))
      done
      if systemctl is-active --quiet "$BINARY_NAME" 2>/dev/null; then
        warn "Service $BINARY_NAME still active after stop; attempting systemctl kill"
        sudo systemctl kill "$BINARY_NAME" --kill-who=all --signal=SIGKILL >/dev/null 2>&1 || warn "systemctl kill failed for $BINARY_NAME"
      fi
      stopped=1
    fi

    # Legacy service name cleanup (ignore failures)
    if systemctl is-active --quiet "cpu-throttle" 2>/dev/null; then
      log "Stopping legacy systemd service: cpu-throttle"
      sudo systemctl stop cpu-throttle >/dev/null 2>&1 || true
      sudo systemctl disable cpu-throttle >/dev/null 2>&1 || true
      sudo rm -f /etc/systemd/system/cpu-throttle.service >/dev/null 2>&1 || true
      sudo systemctl daemon-reload >/dev/null 2>&1 || true
      stopped=1
    fi
  fi

  # If pgrep/pkill are available, try to kill any leftover processes matching the daemon
  if command -v pgrep >/dev/null 2>&1; then
    if pgrep -f "$DEAMON_PATH" >/dev/null 2>&1 || pgrep -f "$BINARY_NAME" >/dev/null 2>&1; then
      log "Terminating running processes for $BINARY_NAME"
      if command -v pkill >/dev/null 2>&1; then
        sudo pkill -f "$BINARY_NAME" >/dev/null 2>&1 || true
      else
        # fallback: kill by PID list
        local pids
        pids="$(pgrep -f "$BINARY_NAME" || true)"
        if [ -n "$pids" ]; then
          for pid in $pids; do
            sudo kill -9 "$pid" >/dev/null 2>&1 || true
          done
        fi
      fi
      # small wait and re-check; if processes persist attempt a stronger kill
      sleep 1
      if pgrep -f "$BINARY_NAME" >/dev/null 2>&1; then
        warn "Processes for $BINARY_NAME still present after pkill; attempting sudo kill -9"
        local pids2
        pids2="$(pgrep -f "$BINARY_NAME" || true)"
        if [ -n "$pids2" ]; then
          for pid in $pids2; do
            sudo kill -9 "$pid" >/dev/null 2>&1 || true
          done
        fi
      fi
      stopped=1
    fi
  fi

  if [ "$stopped" -eq 1 ]; then
    sleep 1
  fi

  # restore errexit
  set -e
}

stop_daemon

# =========================
# Install binaries
# =========================

install_bin_if_present() {
  local name="$1" dest="$2"
  for candidate in "$WORK_DIR/$name" "${WORK_DIR}/${name//_/-}"; do
    if [ -f "$candidate" ]; then
      log "Installing $(basename "$candidate") to $dest"
      if ! sudo install -m 0755 "$candidate" "$dest"; then
        warn "Failed to install $candidate to $dest (continuing)"
      fi
      return 0
    fi
  done
  log "No candidate found for $name"
  return 1
}

BIN_INSTALLED=0
install_bin_if_present "$BINARY_NAME" "$DEAMON_PATH" && BIN_INSTALLED=1
install_bin_if_present "$CTL_BINARY_NAME" "$DEAMON_CTL_PATH" || warn "$CTL_BINARY_NAME not found in staged artifacts."

if [ "$ENABLE_TUI" -eq 1 ]; then
  install_bin_if_present "$TUI_BIN_NAME" "$TUI_BUILD_PATH" || warn "$TUI_BIN_NAME not found."
fi

if [ "$BIN_INSTALLED" -eq 0 ]; then
  warn "Core daemon ($BINARY_NAME) was not found in staged artifacts. Continuing anyway."
fi

# Remove legacy binaries if present
if [ -f "/usr/local/bin/cpu-throttle" ]; then
  log "Removing legacy binary /usr/local/bin/cpu-throttle"
  sudo rm -f /usr/local/bin/cpu-throttle
fi
if [ -f "/usr/local/bin/cpu_throttle-ctl" ]; then
  log "Removing legacy binary /usr/local/bin/cpu_throttle-ctl"
  sudo rm -f /usr/local/bin/cpu_throttle-ctl
fi

# =========================
# Port selection and web API enablement
# =========================

is_port_free() {
  local port="$1"
  if command -v ss >/dev/null 2>&1; then
    ! ss -ltn "( sport = :$port )" | grep -q ":$port"
  elif command -v netstat >/dev/null 2>&1; then
    ! netstat -ltn | awk '{print $4}' | grep -q ":$port$"
  else
    return 0
  fi
}

choose_port() {
  local chosen=""
  if [ "$WEB_ENABLE" -eq 1 ] && [ -n "$WEB_PORT" ]; then
    if is_port_free "$WEB_PORT"; then echo "$WEB_PORT"; return; else warn "Port $WEB_PORT is occupied."; fi
  fi

  if [ "$WEB_ENABLE" -eq 0 ]; then
    if prompt_yes_no "Enable web API for $BINARY_NAME?"; then
      WEB_ENABLE=1
    fi
  fi

  if [ "$WEB_ENABLE" -eq 1 ]; then
    for p in "${DEFAULT_PORTS[@]}"; do
      if is_port_free "$p"; then chosen="$p"; break; fi
    done
    if [ -z "$chosen" ]; then
      local p
      p="$(prompt_input 'Choose a port for web API' '8686')"
      if ! [[ "$p" =~ ^[0-9]{2,5}$ ]]; then err "Invalid port: $p"; exit 1; fi
      if ! is_port_free "$p"; then err "Port $p is occupied."; exit 1; fi
      chosen="$p"
    fi
    # Ensure WEB_ENABLE stays 1 when we found a port
    if [ -n "$chosen" ]; then
      WEB_ENABLE=1
    fi
    echo "$chosen"; return
  fi
  echo ""
}

SELECTED_PORT="$(choose_port | head -n1)"

# If a port was selected, persist it in /etc/cpu_throttle.conf (preferred over unit args)
if [ -n "$SELECTED_PORT" ]; then
  log "Writing web_port=$SELECTED_PORT to /etc/cpu_throttle.conf"
  sudo mkdir -p /etc
  if sudo grep -q "^web_port=" /etc/cpu_throttle.conf 2>/dev/null; then
    sudo sed -i "s/^web_port=.*/web_port=${SELECTED_PORT}/" /etc/cpu_throttle.conf || true
  else
    echo "web_port=${SELECTED_PORT}" | sudo tee -a /etc/cpu_throttle.conf >/dev/null
  fi
fi

# =========================
# Create or refresh systemd service
# =========================

create_service() {
  
  # Backup existing unit if present
  if [ -f "$SERVICE_PATH" ]; then
    sudo cp "$SERVICE_PATH" "${SERVICE_PATH}.backup.$(date +%Y%m%d_%H%M%S)" || warn "Failed to backup existing service unit"
  fi

  local exec_path="$BUILD_PATH"

  local unit="[Unit]
Description=${BINARY_NAME} daemon (temperature-based CPU governor)
After=network.target

[Service]
Type=simple
ExecStart=$exec_path
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
"
  log "Writing systemd service to $SERVICE_PATH"
  echo "$unit" | sudo tee "$SERVICE_PATH" >/dev/null
  sudo systemctl daemon-reload

  if systemctl is-enabled --quiet "$BINARY_NAME"; then
    log "Service already enabled."
  else
    sudo systemctl enable "$BINARY_NAME" || warn "Failed to enable service"
  fi
  sudo systemctl restart "$BINARY_NAME" || warn "Failed to restart service"
}

if command -v systemctl >/dev/null 2>&1; then
  create_service
else
  warn "Systemd not available; skipping service setup."
fi

# =========================
# Firewall configuration
# =========================

configure_firewall() {
  
  [ "$WEB_ENABLE" -eq 1 ] || return 0
  local port="$SELECTED_PORT"
  if [ -z "$port" ]; then return 0; fi

  if prompt_yes_no "Open firewall for port $port?"; then
    if command -v ufw >/dev/null 2>&1; then
      log "Configuring ufw..."
      if sudo ufw status | grep -q "${port}/tcp"; then
        log "ufw: rule for ${port}/tcp already present"
      else
        sudo ufw allow "${port}/tcp" || warn "ufw rule failed."
      fi
    fi

    if command -v firewall-cmd >/dev/null 2>&1; then
      log "Configuring firewalld..."
      if sudo firewall-cmd --permanent --query-port="${port}/tcp" >/dev/null 2>&1; then
        log "firewalld: port ${port}/tcp already allowed"
      else
        sudo firewall-cmd --permanent --add-port="${port}/tcp" || warn "firewalld rule failed."
        sudo firewall-cmd --reload || true
      fi
    fi

    if command -v nft >/dev/null 2>&1; then
      log "Configuring nftables (simple permissive rule)..."
      if sudo nft list ruleset 2>/dev/null | grep -q "dport[[:space:]]\+${port}\b"; then
        log "nftables: rule for dport ${port} already present"
      else
        sudo nft add rule inet filter input tcp dport "$port" accept || warn "nft rule failed."
      fi
      sudo nft list ruleset >/dev/null || true
    elif command -v iptables >/dev/null 2>&1; then
      log "Configuring iptables (simple permissive rule)..."
      if sudo iptables -C INPUT -p tcp --dport "$port" -j ACCEPT >/dev/null 2>&1; then
        log "iptables: rule for port ${port} already present"
      else
        sudo iptables -I INPUT -p tcp --dport "$port" -j ACCEPT || warn "iptables rule failed."
        if command -v netfilter-persistent >/dev/null 2>&1; then
          sudo netfilter-persistent save || true
        fi
      fi
    fi
  fi
}
configure_firewall

# =========================
# Summary
# =========================

log "==============================================================="
log " Installation complete."
log " Binary:        $DEAMON_PATH"
log " Control tool:  $DEAMON_CTL_PATH (if available)"
if [ "$ENABLE_TUI" -eq 1 ]; then
  log " TUI:           $TUI_BUILD_PATH (if available)"
fi
if command -v systemctl >/dev/null 2>&1; then
  log " Service:       $SERVICE_PATH"
  log " Systemd unit:  $BINARY_NAME (enabled and restarted)"
fi
if [ -n "$SELECTED_PORT" ]; then
  log " Web API:       Enabled on port $SELECTED_PORT"
  log " Access:        http://<host>:$SELECTED_PORT/"
else
  log " Web API:       Disabled"
fi
log " Staging dir:   $WORK_DIR"
if [ "$DEBUG_INSTALL" -eq 1 ]; then
  log " Debug artifacts: $DEBUG_DIR"
fi
log "==============================================================="