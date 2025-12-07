#!/usr/bin/env bash
set -euo pipefail

# If executed remotely (via curl), download and execute locally to avoid issues
if [ "${BASH_SOURCE[0]:-}" = "-" ] || [ -z "${BASH_SOURCE[0]:-}" ] || [[ "${BASH_SOURCE[0]}" == http* ]]; then
    echo "Remote execution detected - downloading script locally for proper execution..."
    
    # Create temp directory
    TEMP_DIR=$(mktemp -d)
    SCRIPT_PATH="$TEMP_DIR/install_cpu-throttle.sh"
    
    # Download the script
    if command -v curl >/dev/null 2>&1; then
        curl -fsSL "https://raw.githubusercontent.com/DiabloPower/burn2cool/main/install_cpu-throttle.sh" -o "$SCRIPT_PATH"
    elif command -v wget >/dev/null 2>&1; then
        wget -q "https://raw.githubusercontent.com/DiabloPower/burn2cool/main/install_cpu-throttle.sh" -O "$SCRIPT_PATH"
    else
        echo "ERROR: Neither curl nor wget available for downloading script"
        exit 1
    fi
    
    # Make executable and run locally
    chmod +x "$SCRIPT_PATH"
    # Mark that this is a temp execution for cleanup
    export BURN2COOL_TEMP_EXEC=1
    export BURN2COOL_TEMP_DIR="$TEMP_DIR"
    exec "$SCRIPT_PATH" "$@"
    exit 0
fi

# Ignore SIGTERM to prevent remote termination
trap 'log "Received SIGTERM - ignoring to prevent premature termination"' TERM

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

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

log "=============================================================================="
log " Burn2Cool installer for ${BINARY_NAME} (temperature-based CPU governor)"
log " Repo: https://github.com/${REPO_OWNER}/${REPO_NAME}"
log " This will:"
log "  - Interactively select components (daemon, ctl, tui, gui, web API, firewall)"
log "  - Download prebuilt binaries or build from source"
log "  - Install dependencies automatically (if supported)"
log "  - Stage files in a temp dir for processing"
log "  - Stop any running services and install new binaries"
log "  - Configure web API and firewall (if selected)"
log "  - Install GUI components and create desktop entries"
log "  - Create/refresh systemd service and start it"
log "=============================================================================="

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
# Interactive component selection
# =========================
# Single-select menu helper (keyboard interactive)
_singleselect_menu() {
  local return_value=$1
  local -n opt_ref=$2
  local -n def_ref=$3
  # nameref to caller's return variable
  local -n out_ref=$1

  stty -echo

  __cursor_blink_on() { printf "\e[?25h"; }
  __cursor_blink_off() { printf "\e[?25l"; }
  __cursor_to() { local row=$1; local col=${2:-1}; printf "\e[%s;%sH" "$row" "$col"; }
  __get_cursor_row() { local row="" col=""; IFS=";" read -rs -d "R" -p $'\E[6n' row col; printf "%s" "${row#*[}"; }
  __get_keyboard_command() {
    local key=""
    IFS="" read -rs -n 1 key &>/dev/null
    case "$key" in
    "") printf "enter" ;;
    " ") printf "select" ;;
    "q"|"Q") printf "quit" ;;
    $'\e') IFS="" read -rs -n 2 key &>/dev/null; case "$key" in
      "[A"|"[D") printf "up" ;;
      "[B"|"[C") printf "down" ;;
    esac ;;
    esac
  }

  __on_ctrl_c() { __cursor_to "$last_row"; __cursor_blink_on; stty echo; exit 1; }
  trap "__on_ctrl_c" SIGINT

  local selected=-1
  # Default: first true entry
  for i in "${!def_ref[@]}"; do
    if [[ ${def_ref[i]} == "true" ]]; then selected=$i; break; fi
  done
  ((selected < 0)) && selected=0

  # Reserve lines for each option so cursor positioning is stable (like _multiselect_menu)
  for i in "${!opt_ref[@]}"; do printf "\n"; done
  local last_row=$(__get_cursor_row)
  local start_row=$((last_row - ${#opt_ref[@]}))

  __print_options() {
    local active=$1
    for i in "${!opt_ref[@]}"; do
      __cursor_to "$((start_row + i))"
      printf "("
      if ((i == ${selected:--1})); then
        printf "\e[1;32m*\e[0m"
      else
        printf " "
      fi
      printf ") "
      if ((i == active)); then
        printf "\e[7m"
      fi
      printf "${opt_ref[i]:-}"
      if ((i == active)); then
        printf "\e[27m"
      fi
      printf "\n"
    done
  }

  __cursor_blink_off
  local active=0
  while true; do
    __print_options "$active"
    case $(__get_keyboard_command) in
      "enter") break ;;
      "select") selected=$active ;;
      "up") ((active--)); ((active<0)) && active=$((${#opt_ref[@]}-1)) ;;
      "down") ((active++)); ((active>=${#opt_ref[@]})) && active=0 ;;
      "quit") stty echo; __cursor_blink_on; exit 1 ;;
    esac
  done

  __cursor_to "$last_row"; __cursor_blink_on; stty echo
  out_ref=$selected
  trap "" SIGINT
}

# Multi-select menu helper (keyboard interactive)
_multiselect_menu() {
  local return_value=$1
  local -n opt_ref=$2
  local -n def_ref=$3
  # nameref to caller's return array variable (so we can assign directly)
  local -n out_ref=$1

  stty -echo

  __cursor_blink_on() { printf "\e[?25h"; }
  __cursor_blink_off() { printf "\e[?25l"; }
  __cursor_to() { local row=$1; local col=${2:-1}; printf "\e[%s;%sH" "$row" "$col"; }
  __get_cursor_row() { local row="" col=""; IFS=";" read -rs -d "R" -p $'\E[6n' row col; printf "%s" "${row#*[}"; }
  __get_keyboard_command() {
    local key=""
    IFS="" read -rs -n 1 key &>/dev/null
    case "$key" in
    "") printf "enter" ;;
    " ") printf "toggle_active" ;;
    "a"|"A") printf "toggle_all" ;;
    "q"|"Q") printf "quit" ;;
    $'\e') IFS="" read -rs -n 2 key &>/dev/null; case "$key" in
      "[A"|"[D") printf "up" ;;
      "[B"|"[C") printf "down" ;;
    esac ;;
    esac
  }
  __on_ctrl_c() { __cursor_to "$last_row"; __cursor_blink_on; stty echo; exit 1; }
  trap "__on_ctrl_c" SIGINT

  out_ref=()
  local i=0
  for i in "${!opt_ref[@]}"; do
    if [[ ${def_ref[i]} == "false" ]]; then out_ref+=("false"); else out_ref+=("true"); fi
    printf "\n"
  done

  local start_row="" last_row=""
  last_row=$(__get_cursor_row)
  start_row=$((last_row - ${#opt_ref[@]}))

  __print_options() {
    local index_active=$1
    local i=0
    for i in "${!opt_ref[@]}"; do
      local prefix="[ ]"
      if [[ ${out_ref[i]} == "true" ]]; then prefix="[\e[1;32m*\e[0m]"; fi
      __cursor_to "$((start_row + i))"
      local option="${opt_ref[i]:-}"
      if ((i == index_active)); then
        printf "$prefix \e[7m$option\e[27m"
      else
        printf "$prefix $option"
      fi
      printf "\n"
    done
  }

  __cursor_blink_off
  local active=0
  while true; do
    __print_options "$active"
    case $(__get_keyboard_command) in
    "enter") __print_options -1; break ;;
    "toggle_active") if [[ ${out_ref[active]} == "true" ]]; then out_ref[active]="false"; else out_ref[active]="true"; fi ;;
    "toggle_all") local i=0; if [[ ${out_ref[active]} == "true" ]]; then for i in "${!out_ref[@]}"; do out_ref[i]="false"; done; else for i in "${!out_ref[@]}"; do out_ref[i]="true"; done; fi ;;
    "quit") __on_ctrl_c ;;
    "up") active=$((active - 1)); if [[ $active -lt 0 ]]; then active=$((${#opt_ref[@]} - 1)); fi ;;
    "down") active=$((active + 1)); if [[ $active -ge ${#opt_ref[@]} ]]; then active=0; fi ;;
    esac
  done

  __cursor_to "$last_row"
  __cursor_blink_on
  stty echo
  trap "" SIGINT
}

# Present interactive component selection and populate WANT_* globals
component_selection() {
  # Defaults: select all when non-interactive flag is set
  local options=("daemon (cpu_throttle)" "ctl (cpu_throttle_ctl)" "tui (cpu_throttle_tui)" "gui (GUI zip)" "--- Settings ---" "web API" "firewall ports" "systemd service")
  local defaults=("true" "true" "true" "true" "false" "false" "false" "false")
  local selected=()
  if [ "${YES:-0}" -eq 1 ] || [ "${AUTO_YES:-0}" -eq 1 ]; then
    # keep defaults true
    selected=("true" "true" "true" "true" "false" "true" "true" "true")
  else
    # Offer a quick single-select first (download mode), then component multiselect
    printf "Select download mode:\nUse arrow keys to navigate, Space to toggle, Enter to confirm, q to quit.\n\n"
    local mode_choice=0
    local mode_options=("Precompiled binary" "Build from source")
    local mode_defaults=("true" "false")
    _singleselect_menu mode_choice mode_options mode_defaults
    if [ "${mode_choice:-0}" -eq 0 ]; then
      PRESELECTED_MODE="binaries"
    else
      PRESELECTED_MODE="source"
    fi
    # Clear screen after single select
    printf "\e[2J\e[H"
    printf "Select components:\nUse arrow keys to navigate, Space to toggle, Enter to confirm, q to quit.\n\n"
    _multiselect_menu selected options defaults
  fi

  WANT_DAEMON=0
  WANT_CTL=0
  WANT_TUI=0
  WANT_GUI=0
  WANT_WEB=0
  WANT_FIREWALL=0
  WANT_SERVICE=0
  if [ "${selected[0]}" = "true" ]; then WANT_DAEMON=1; fi
  if [ "${selected[1]}" = "true" ]; then WANT_CTL=1; fi
  if [ "${selected[2]}" = "true" ]; then WANT_TUI=1; fi
  if [ "${selected[3]}" = "true" ]; then WANT_GUI=1; fi
  if [ "${selected[5]}" = "true" ]; then WANT_WEB=1; fi
  if [ "${selected[6]}" = "true" ]; then WANT_FIREWALL=1; fi
  if [ "${selected[7]}" = "true" ]; then WANT_SERVICE=1; fi

  log "Component selection: daemon=$WANT_DAEMON ctl=$WANT_CTL tui=$WANT_TUI gui=$WANT_GUI web=$WANT_WEB firewall=$WANT_FIREWALL service=$WANT_SERVICE"
}

# Run selection now so subsequent download logic can consult WANT_* flags
set +e
component_selection
set -e

# =========================
# Asset selection logic
# =========================

choose_mode() {
  # If an interactive preselection was made earlier, prefer it
  if [ -n "${PRESELECTED_MODE:-}" ]; then
    echo "$PRESELECTED_MODE"; return
  fi
  if [ -n "$FETCH_URL" ]; then echo "fetch"; return; fi
  if [ "$FORCE_BUILD" -eq 1 ] && [ "$FORCE_BINARIES" -eq 1 ]; then
    warn "Both --force-build and --force-binaries set; defaulting to binaries."
    echo "binaries"; return
  fi
  # If GUI is selected, force source mode (GUI needs source to build)
  if [ "${WANT_GUI:-0}" -eq 1 ]; then
    echo "source"; return
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
  # Build desired patterns from interactive selection
  local patterns=()
  if [ "${WANT_DAEMON:-0}" -eq 1 ]; then patterns+=("$BINARY_NAME"); fi
  if [ "${WANT_CTL:-0}" -eq 1 ]; then patterns+=("$CTL_BINARY_NAME"); fi
  if [ "${WANT_TUI:-0}" -eq 1 ]; then patterns+=("$TUI_BIN_NAME"); fi
  if [ "${WANT_GUI:-0}" -eq 1 ] && [ "${GUI_BUILT:-0}" -eq 0 ]; then patterns+=("gui" "tray" "burn2cool_tray" "gui_tray"); fi
  for i in "${!names[@]}"; do
    n="${names[$i]}"; u="${urls[$i]}"
    # skip scripts/build metadata
    if [[ "$n" =~ \.sh$ ]] || [[ "$n" =~ autobuild ]]; then
      continue
    fi
    # explicit GUI asset name (exact) support
    if [ "${WANT_GUI:-0}" -eq 1 ] && [ "${GUI_BUILT:-0}" -eq 0 ] && [ "$n" = "burn2cool_tray.zip" ]; then
      download_url "$u" "$WORK_DIR/$n"
      selected=$((selected+1))
      continue
    fi
    # check if name matches any desired pattern
    local match=0
    for p in "${patterns[@]}"; do
      if [[ -n "$p" ]] && [[ "$n" == "$p" ]]; then match=1; break; fi
    done
    if [ "$match" -eq 0 ]; then
      continue
    fi
    # download: archives allowed for GUI (zip/tar), binaries get executable bit
    if [[ "$n" =~ \.(zip|tar\.gz|tgz)$ ]]; then
      download_url "$u" "$WORK_DIR/$n"
    else
      download_url "$u" "$WORK_DIR/$n"
      chmod +x "$WORK_DIR/$n" || true
    fi
    selected=$((selected+1))
  done

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

  # Build GUI if selected
  if [ "${WANT_GUI:-0}" -eq 1 ]; then
    log "Building GUI component..."
    # Check and install GUI dependencies
    local gui_deps=("libgtk-3-dev" "libayatana-appindicator3-dev" "libcurl4-openssl-dev" "libjson-c-dev" "libnotify-dev")
    local missing_gui_deps=()
    if ! pkg-config --exists gtk+-3.0 2>/dev/null; then missing_gui_deps+=("libgtk-3-dev"); fi
    if ! pkg-config --exists ayatana-appindicator3-0.1 2>/dev/null && ! pkg-config --exists libappindicator-3.0 2>/dev/null; then missing_gui_deps+=("libayatana-appindicator3-dev"); fi
    if ! pkg-config --exists libcurl 2>/dev/null; then missing_gui_deps+=("libcurl4-openssl-dev"); fi
    if ! pkg-config --exists json-c 2>/dev/null; then missing_gui_deps+=("libjson-c-dev"); fi
    if ! pkg-config --exists libnotify 2>/dev/null; then missing_gui_deps+=("libnotify-dev"); fi
    if [ ${#missing_gui_deps[@]} -gt 0 ]; then
      log "Installing GUI dependencies: ${missing_gui_deps[*]}"
      install_deps "${missing_gui_deps[@]}"
    fi
    if [ -d "$src_dir/gui_tray" ]; then
      local MAKE_SHELL_ARG=""
      if command -v bash >/dev/null 2>&1; then
        MAKE_SHELL_ARG="SHELL=/bin/bash"
      fi
      (cd "$src_dir/gui_tray" && make $MAKE_SHELL_ARG clean && make $MAKE_SHELL_ARG)
      if [ -f "$src_dir/gui_tray/burn2cool_tray" ]; then
        install -m 0755 "$src_dir/gui_tray/burn2cool_tray" "$WORK_DIR/burn2cool_tray"
        mkdir -p "$WORK_DIR/staging"
        cp -r "$src_dir/gui_tray/assets" "$src_dir/gui_tray/i18n" "$WORK_DIR/staging/" 2>/dev/null || true
        GUI_BUILT=1
      else
        warn "GUI build failed - binary not found"
      fi
    else
      warn "gui_tray directory not found in source"
    fi
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

  if [ "$ENABLE_TUI" -eq 1 ] || [ "${WANT_TUI:-0}" -eq 1 ]; then
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

  if [ "$built" -eq 0 ] && [ "${GUI_BUILT:-0}" -eq 0 ]; then
    err "No suitable binary assets found in the release."
    return 1
  fi

  # Remove unwanted binaries based on selection
  if [ "${WANT_DAEMON:-0}" -eq 0 ] && [ -f "$WORK_DIR/$BINARY_NAME" ]; then
    log "Removing unwanted daemon binary ($BINARY_NAME)"
    rm -f "$WORK_DIR/$BINARY_NAME"
  fi
  if [ "${WANT_CTL:-0}" -eq 0 ] && [ -f "$WORK_DIR/$CTL_BINARY_NAME" ]; then
    log "Removing unwanted ctl binary ($CTL_BINARY_NAME)"
    rm -f "$WORK_DIR/$CTL_BINARY_NAME"
  fi
  if [ "${WANT_TUI:-0}" -eq 0 ] && [ -f "$WORK_DIR/$TUI_BIN_NAME" ]; then
    log "Removing unwanted tui binary ($TUI_BIN_NAME)"
    rm -f "$WORK_DIR/$TUI_BIN_NAME"
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
# GUI zip handling: unpack, install into a dedicated dir, create symlink, desktop file and uninstall helper
# =========================
unpack_and_install_gui_zip() {
  if [ "${WANT_GUI:-0}" -ne 1 ]; then return 0; fi
  local zip="$WORK_DIR/burn2cool_tray.zip"
  [ -f "$zip" ] || return 0

  log "Found GUI asset: $zip — preparing to install GUI files"

  # ensure unzip is available
  if ! command -v unzip >/dev/null 2>&1; then
    warn "'unzip' not found — GUI zip cannot be unpacked. Skipping GUI installation."
    return 1
  fi

  local staging="$WORK_DIR/gui_staging"
  rm -rf "$staging"
  mkdir -p "$staging"
  unzip -q "$zip" -d "$staging"

  # Determine extracted root: if single top-level dir, use it
  local roots
  roots=("$(find "$staging" -mindepth 1 -maxdepth 1 -printf '%f\n')")
  local srcdir
  if [ $(find "$staging" -mindepth 1 -maxdepth 1 | wc -l) -eq 1 ]; then
    srcdir="$staging/${roots[0]}"
  else
    srcdir="$staging"
  fi

  # Decide install prefix: prefer system path, fallback to user-local
  local system_share="/usr/local/share/${REPO_NAME}_tray"
  local user_share="$HOME/.local/share/${REPO_NAME}_tray"
  local install_dir=""

  if sudo -n true >/dev/null 2>&1; then
    install_dir="$system_share"
  else
    # try to create with sudo; if not possible, use user dir
    if mkdir -p "$system_share" >/dev/null 2>&1; then
      install_dir="$system_share"
    else
      install_dir="$user_share"
    fi
  fi

  log "Installing GUI into: $install_dir"
  if [ "$install_dir" = "$system_share" ]; then
    sudo mkdir -p "$install_dir"
    sudo cp -a "$srcdir/." "$install_dir/"
    sudo chown -R root:root "$install_dir" || true
  else
    mkdir -p "$install_dir"
    cp -a "$srcdir/." "$install_dir/"
  fi

  # Find executable binary inside install_dir
  local binpath
  binpath=$(find "$install_dir" -type f -name "burn2cool_tray" -perm /u=x,g=x,o=x | head -n1 || true)
  if [ -z "$binpath" ]; then
    # Try common locations
    if [ -x "$install_dir/burn2cool_tray" ]; then binpath="$install_dir/burn2cool_tray"; fi
    if [ -z "$binpath" ]; then
      binpath=$(find "$install_dir" -type f -name "burn2cool_tray" | head -n1 || true)
    fi
  fi

  # Create symlink in bin path (system or user)
  local symlink_target
  if [ "$install_dir" = "$system_share" ]; then
    symlink_target="/usr/local/bin/burn2cool_tray"
    if [ -n "$binpath" ]; then
      sudo ln -sf "$binpath" "$symlink_target"
    fi
  else
    mkdir -p "$HOME/.local/bin"
    symlink_target="$HOME/.local/bin/burn2cool_tray"
    if [ -n "$binpath" ]; then
      ln -sf "$binpath" "$symlink_target"
    fi
  fi

  log "Created symlink: $symlink_target -> $binpath"

  # Install desktop file (user-local preferred)
  local desktop_dir
  if [ "$install_dir" = "$system_share" ]; then desktop_dir="/usr/share/applications"; else desktop_dir="$HOME/.local/share/applications"; fi
  mkdir -p "$desktop_dir"
  local icon_path="$install_dir/icon.png"
  if [ -f "$install_dir/assets/icon.png" ]; then icon_path="$install_dir/assets/icon.png"; fi

  local desktop_file="$desktop_dir/burn2cool-tray.desktop"
  cat > "$WORK_DIR/burn2cool-tray.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=Burn2Cool Tray
Exec=$symlink_target
Icon=$icon_path
Terminal=false
Categories=Utility;System;
EOF

  if [ "$desktop_dir" = "/usr/share/applications" ]; then
    sudo mv "$WORK_DIR/burn2cool-tray.desktop" "$desktop_file"
  else
    mv "$WORK_DIR/burn2cool-tray.desktop" "$desktop_file"
  fi

  log "Installed desktop file: $desktop_file"

  # Offer autostart (user-level)
  if prompt_yes_no "Enable GUI autostart for the current user?"; then
    local autostart_dir="$HOME/.config/autostart"
    mkdir -p "$autostart_dir"
    cp "$desktop_file" "$autostart_dir/"
    log "Autostart enabled: $autostart_dir/$(basename $desktop_file)"
  fi

  # Create an uninstall helper script inside install_dir
  local uninstall_sh="$install_dir/uninstall_burn2cool_tray.sh"
  cat > "$WORK_DIR/uninstall_burn2cool_tray.sh" <<'UNS'
#!/usr/bin/env bash
set -euo pipefail
INSTALL_DIR="__INSTALL_DIR__"
SYMLINK="__SYMLINK__"
DESKTOP="__DESKTOP__"
echo "Removing GUI installation from $INSTALL_DIR"
if [ -n "$SYMLINK" ] && [ -L "$SYMLINK" ]; then
  sudo rm -f "$SYMLINK" || true
fi
rm -rf "$INSTALL_DIR" || true
rm -f "$DESKTOP" || true
rm -f "$HOME/.config/autostart/$(basename "$DESKTOP")" || true
echo "Uninstall complete"
UNS
  # substitute paths
  sed -e "s|__INSTALL_DIR__|$install_dir|g" -e "s|__SYMLINK__|$symlink_target|g" -e "s|__DESKTOP__|$desktop_file|g" "$WORK_DIR/uninstall_burn2cool_tray.sh" > "$WORK_DIR/uninstall_burn2cool_tray.sh.tmp"
  mv "$WORK_DIR/uninstall_burn2cool_tray.sh.tmp" "$WORK_DIR/uninstall_burn2cool_tray.sh"
  chmod +x "$WORK_DIR/uninstall_burn2cool_tray.sh"
  if [ "$install_dir" = "$system_share" ]; then
    sudo mv "$WORK_DIR/uninstall_burn2cool_tray.sh" "$install_dir/uninstall_burn2cool_tray.sh"
  else
    mv "$WORK_DIR/uninstall_burn2cool_tray.sh" "$install_dir/uninstall_burn2cool_tray.sh"
  fi

  log "Created uninstall helper at: $install_dir/uninstall_burn2cool_tray.sh"
}

# Run GUI unpack/install if GUI asset present
unpack_and_install_gui_zip || true

# =========================
# Stop running daemon if present
# =========================

stop_daemon() {
  # Only stop if daemon is being installed
  if [ "${WANT_DAEMON:-0}" -eq 0 ]; then return; fi

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
        warn "Service $BINARY_NAME still active after stop; skipping kill to avoid killing the installer"
        # sudo systemctl kill "$BINARY_NAME" --kill-who=all --signal=SIGKILL >/dev/null 2>&1 || warn "systemctl kill failed for $BINARY_NAME"
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
    if pgrep -f "$DEAMON_PATH" >/dev/null 2>&1 || pgrep "^$BINARY_NAME$" >/dev/null 2>&1; then
      log "Terminating running processes for $BINARY_NAME"
      if command -v pkill >/dev/null 2>&1; then
        # Use more specific pattern to avoid killing the installer script itself
        sudo pkill -f "$DEAMON_PATH" >/dev/null 2>&1 || true
        # Only kill processes that are exactly the binary name, not containing it
        sudo pkill "^$BINARY_NAME$" >/dev/null 2>&1 || true
      else
        # fallback: kill by PID list
        local pids
        pids="$(pgrep -f "$DEAMON_PATH" || pgrep "^$BINARY_NAME$" || true)"
        if [ -n "$pids" ]; then
          for pid in $pids; do
            sudo kill -9 "$pid" >/dev/null 2>&1 || true
          done
        fi
      fi
      # small wait and re-check; if processes persist attempt a stronger kill
      sleep 1
      if pgrep -f "$DEAMON_PATH" >/dev/null 2>&1 || pgrep "^$BINARY_NAME$" >/dev/null 2>&1; then
        warn "Processes for $BINARY_NAME still present after pkill; attempting sudo kill -9"
        local pids2
        pids2="$(pgrep -f "$DEAMON_PATH" || pgrep "^$BINARY_NAME$" || true)"
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
if [ "${WANT_DAEMON:-0}" -eq 1 ]; then
  install_bin_if_present "$BINARY_NAME" "$DEAMON_PATH" && BIN_INSTALLED=1
fi
if [ "${WANT_CTL:-0}" -eq 1 ]; then
  install_bin_if_present "$CTL_BINARY_NAME" "$DEAMON_CTL_PATH" || warn "$CTL_BINARY_NAME not found in staged artifacts."
fi

if [ "$ENABLE_TUI" -eq 1 ] || [ "${WANT_TUI:-0}" -eq 1 ]; then
  install_bin_if_present "$TUI_BIN_NAME" "$TUI_BUILD_PATH" || warn "$TUI_BIN_NAME not found."
fi

if [ "${WANT_GUI:-0}" -eq 1 ]; then
  # In source mode, install from staging; in binary mode, unpack_and_install_gui_zip already did it
  if [ "${GUI_BUILT:-0}" -eq 1 ]; then
    # Install to share directory like binary mode
    gui_install_dir="/usr/local/share/burn2cool_tray"
    sudo mkdir -p "$gui_install_dir"
    sudo install -m 0755 "$WORK_DIR/burn2cool_tray" "$gui_install_dir/burn2cool_tray"
    sudo ln -sf "$gui_install_dir/burn2cool_tray" "/usr/local/bin/burn2cool_tray"
    log "Created symlink: /usr/local/bin/burn2cool_tray -> $gui_install_dir/burn2cool_tray"
    # Create desktop file
    cat << EOF | sudo tee /usr/share/applications/burn2cool-tray.desktop >/dev/null
[Desktop Entry]
Name=Burn2Cool Tray
Exec=/usr/local/bin/burn2cool_tray
Icon=burn2cool
Type=Application
Categories=System;Monitor;
EOF
    log "Installed desktop file: /usr/share/applications/burn2cool-tray.desktop"
    # Install icon
    sudo mkdir -p /usr/share/icons/hicolor/48x48/apps
    sudo cp "$WORK_DIR/staging/assets/icon.png" /usr/share/icons/hicolor/48x48/apps/burn2cool.png
    sudo gtk-update-icon-cache /usr/share/icons/hicolor 2>/dev/null || true
    log "Installed application icon"
    # Ask about autostart
    if prompt_yes_no "Enable GUI autostart for the current user?" "n"; then
      mkdir -p ~/.config/autostart
      cp /usr/share/applications/burn2cool-tray.desktop ~/.config/autostart/
      log "Enabled autostart for burn2cool_tray"
    fi
    # Create uninstall script
    cat << 'EOF' | sudo tee "$gui_install_dir/uninstall_burn2cool_tray.sh" >/dev/null
#!/bin/bash
set -e

echo "Uninstalling burn2cool_tray..."

# Remove binary and symlink
sudo rm -f /usr/local/bin/burn2cool_tray
sudo rm -f /usr/local/share/burn2cool_tray/burn2cool_tray

# Remove assets
sudo rm -rf /usr/local/share/burn2cool_tray

# Remove desktop file
sudo rm -f /usr/share/applications/burn2cool-tray.desktop

# Remove icon
sudo rm -f /usr/share/icons/hicolor/48x48/apps/burn2cool.png
sudo gtk-update-icon-cache /usr/share/icons/hicolor 2>/dev/null || true

# Remove autostart if exists
rm -f ~/.config/autostart/burn2cool-tray.desktop

echo "Uninstallation complete."
EOF
    sudo chmod +x "$gui_install_dir/uninstall_burn2cool_tray.sh"
    log "Created uninstall helper at: $gui_install_dir/uninstall_burn2cool_tray.sh"
  fi
  # Install assets if built from source
  if [ "${GUI_BUILT:-0}" -eq 1 ]; then
    sudo mkdir -p /usr/local/share/burn2cool_tray
    sudo cp -r "$WORK_DIR/staging/assets" /usr/local/share/burn2cool_tray/ || warn "Failed to install GUI assets"
    sudo cp -r "$WORK_DIR/staging/i18n" /usr/local/share/burn2cool_tray/ || warn "Failed to install GUI i18n"
  fi
fi

if [ "${WANT_DAEMON:-0}" -eq 1 ] && [ "$BIN_INSTALLED" -eq 0 ]; then
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

  if [ "$WEB_ENABLE" -eq 0 ] && [ "${WANT_WEB:-0}" -eq 1 ]; then
    WEB_ENABLE=1
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
    log "Daemon service already enabled."
  else
    sudo systemctl enable "$BINARY_NAME" || warn "Failed to enable service"
  fi
  # Always restart the service after installation
  sudo systemctl restart "$BINARY_NAME" || warn "Failed to restart service"
}

if command -v systemctl >/dev/null 2>&1 && [ "${WANT_SERVICE:-0}" -eq 1 ]; then
  create_service
elif [ "${WANT_SERVICE:-0}" -eq 1 ]; then
  warn "Systemd not available; skipping service setup."
fi

# =========================
# Firewall configuration
# =========================

configure_firewall() {
  
  [ "$WEB_ENABLE" -eq 1 ] || return 0
  local port="$SELECTED_PORT"
  if [ -z "$port" ]; then return 0; fi

  if [ "${WANT_FIREWALL:-0}" -eq 1 ]; then
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
if [ "${WANT_DAEMON:-0}" -eq 1 ]; then
  log " Binary:        $DEAMON_PATH"
else
  log " Binary:        Not selected"
fi
if [ "${WANT_CTL:-0}" -eq 1 ]; then
  log " Control tool:  $DEAMON_CTL_PATH"
else
  log " Control tool:  Not selected"
fi
if [ "$ENABLE_TUI" -eq 1 ] || [ "${WANT_TUI:-0}" -eq 1 ]; then
  log " TUI:           $TUI_BUILD_PATH"
fi
if [ "${WANT_GUI:-0}" -eq 1 ]; then
  log " GUI:           /usr/local/bin/burn2cool_tray"
fi
if command -v systemctl >/dev/null 2>&1 && [ "${WANT_SERVICE:-0}" -eq 1 ]; then
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

log "Script finished successfully"

# Cleanup temp directory if this was a remote execution
if [ "${BURN2COOL_TEMP_EXEC:-0}" -eq 1 ] && [ -n "${BURN2COOL_TEMP_DIR:-}" ]; then
    log "Cleaning up temporary files..."
    rm -rf "$BURN2COOL_TEMP_DIR" || warn "Failed to cleanup temp directory: $BURN2COOL_TEMP_DIR"
fi