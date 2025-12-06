#!/usr/bin/env bash
set -euo pipefail

# CLI options (can be used for non-interactive installs)
AUTO_YES=0
ENABLE_TUI=0
OVERRIDE_PORT=""
FETCH_URL_OVERRIDE=""
FORCE_BUILD=0
FORCE_BINARIES=0
INSTALL_TUI=0

# Release configuration: by default installer will try to use the latest
# release binary from GitHub Releases. If you want to pin a tag, set
# `RELEASE_TAG` to something like "v3.0". Change `PREFERRED_ASSET_NAME`
# if your release asset has a different filename.
RELEASE_TAG=""                # empty = use latest
PREFERRED_ASSET_NAME="cpu_throttle"  # asset name to look for in release assets

ORIG_ARGC=$#
while [[ $# -gt 0 ]]; do
    case "$1" in
        -y|--yes|--non-interactive)
            AUTO_YES=1; shift ;;
        --tui|--enable-tui)
            ENABLE_TUI=1; shift ;;
        --force-build)
            FORCE_BUILD=1; shift ;;
        --force-binaries)
            FORCE_BINARIES=1; shift ;;
        --install-tui)
            INSTALL_TUI=1; shift ;;
        --port)
            OVERRIDE_PORT="$2"; shift 2 ;;
        --fetch-url)
            FETCH_URL_OVERRIDE="$2"; shift 2 ;;
        -h|--help)
            cat <<USAGE
Usage: $0 [options]
Options:
  -y, --yes, --non-interactive   Run with defaults (accept prompts)
  --tui, --enable-tui            Build and install ncurses TUI (requires ncurses)
  --port <n>                     Use specific port for embedded web API
    --fetch-url <url>              Use a specific remote URL to fetch sources
    --force-build                   Always build from source (do not use prebuilt binaries)
    --force-binaries                Always install prebuilt binaries if available
    -h, --help                     Show this help
USAGE
            exit 0
            ;;
        *)
            echo "Unknown option: $1"; exit 1 ;;
    esac
done

# Interactive installer for cpu_throttle
# - can fetch release from a server (tarball/zip)
# - offers to install build dependencies (asks first)
# - builds using Makefile if present, otherwise compiles main sources
# - asks whether to enable web API and configure firewall
# - creates a systemd service and starts/enables it

BINARY_NAME="cpu_throttle"
CTL_BINARY_NAME="cpu_throttle_ctl"
SOURCE_FILE="cpu_throttle.c"
CTL_SOURCE_FILE="cpu_throttle_ctl.c"
BUILD_PATH="/usr/local/bin/$BINARY_NAME"
CTL_BUILD_PATH="/usr/local/bin/$CTL_BINARY_NAME"
SERVICE_PATH="/etc/systemd/system/$BINARY_NAME.service"
TUI_BIN_NAME="cpu_throttle_tui"
TUI_BUILD_PATH="/usr/local/bin/$TUI_BIN_NAME"

cleanup() {
    if [[ -n "${TMPDIR:-}" && -d "$TMPDIR" ]]; then
        rm -rf "$TMPDIR"
    fi
}
trap cleanup EXIT

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

install_package() {
    local pkgs=("$@")
    case "$PKG_MANAGER" in
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
        pacman)
            sudo pacman -Sy --noconfirm "${pkgs[@]}"
            ;;
        zypper)
            sudo zypper install -y "${pkgs[@]}"
            ;;
        apk)
            sudo apk add --no-cache "${pkgs[@]}"
            ;;
        *)
            echo "No supported package manager detected; cannot install ${pkgs[*]}." >&2
            return 1
            ;;
    esac
}

# Check whether a package is already installed for the detected package manager
has_package() {
    local pkg="$1"
    case "$PKG_MANAGER" in
        apt-get)
            dpkg -s "$pkg" >/dev/null 2>&1 && return 0 || return 1
            ;;
        dnf|yum)
            rpm -q "$pkg" >/dev/null 2>&1 && return 0 || return 1
            ;;
        pacman)
            pacman -Qi "$pkg" >/dev/null 2>&1 && return 0 || return 1
            ;;
        zypper)
            rpm -q "$pkg" >/dev/null 2>&1 && return 0 || return 1
            ;;
        apk)
            apk info -e "$pkg" >/dev/null 2>&1 && return 0 || return 1
            ;;
        *)
            return 1
            ;;
    esac
}

# Detect whether ncurses headers/libraries are already available
has_ncurses() {
    # Prefer pkg-config
    if command -v pkg-config &>/dev/null && pkg-config --exists ncurses; then
        return 0
    fi
    # Try compiling a tiny test program that includes <ncurses.h>
    if command -v gcc &>/dev/null; then
        if printf '%s
' '#include <ncurses.h>' 'int main(void){return 0;}' | gcc -x c - -o /dev/null - >/dev/null 2>&1; then
            return 0
        fi
    fi
    return 1
}

# Show welcome banner when invoked with no args (interactive default)
if [[ "$ORIG_ARGC" -eq 0 && "${QUIET:-0}" -ne 1 ]]; then
    cat <<'BANNER'
============================================================
cpu_throttle installer - interactive

No options passed. You will be asked whether to fetch prebuilt
binaries or build from source. Use -y to run non-interactively.

Examples:
    ./install_cpu-throttle.sh           # interactive
    ./install_cpu-throttle.sh -y        # non-interactive defaults
============================================================
BANNER
fi

# --- Optionally fetch files from server ---
if [[ "$AUTO_YES" -eq 1 ]]; then
    FETCH_ANS=Y
else
    # Default to Yes per user preference (press Enter = Yes)
    read -r -p "Fetch source files from remote server? [Y/n]: " FETCH_ANS
    FETCH_ANS=${FETCH_ANS:-Y}
fi
if [[ "$FETCH_ANS" =~ ^[Yy] ]]; then
    if [[ -n "$FETCH_URL_OVERRIDE" ]]; then
        REMOTE_URL="$FETCH_URL_OVERRIDE"
    else
        # If no explicit URL provided, try to detect latest release asset from GitHub
        detect_release_asset() {
            local api_url
            if [[ -n "${RELEASE_TAG:-}" ]]; then
                api_url="https://api.github.com/repos/DiabloPower/burn2cool/releases/tags/${RELEASE_TAG}"
            else
                api_url="https://api.github.com/repos/DiabloPower/burn2cool/releases/latest"
            fi
            if ! command -v curl >/dev/null 2>&1; then
                return 1
            fi
            # Fetch release JSON and extract browser_download_url lines
            local urls
            urls=$(curl -fsSL "$api_url" 2>/dev/null | sed -n 's/.*"browser_download_url": *"\([^"]*\)".*/\1/p') || return 1
            if [[ -z "$urls" ]]; then
                return 1
            fi
                # Prefer asset names that match PREFERRED_ASSET_NAME
                local match
                # If the user requested a forced build, prefer source archives
                if [[ "${FORCE_BUILD:-0}" -eq 1 ]]; then
                    match=$(echo "$urls" | grep -iE '\.(tar\.gz|tar\.bz2|tar\.xz|zip)$' | head -n1 || true)
                    if [[ -n "$match" ]]; then
                        echo "$match"
                        return 0
                    fi
                    # also prefer repository archive endpoints if present
                    match=$(echo "$urls" | grep -iE '/archive/' | head -n1 || true)
                    if [[ -n "$match" ]]; then
                        echo "$match"
                        return 0
                    fi
                fi
                # Prefer an exact asset name (URL ending with the asset name)
                match=$(echo "$urls" | grep -iE "/${PREFERRED_ASSET_NAME}$" | head -n1 || true)
                if [[ -n "$match" ]]; then
                    echo "$match"
                    return 0
                fi
                # Otherwise fall back to first asset URL
                echo "$(echo "$urls" | head -n1)"
                return 0
        }

        auto_url=""
        # If the user explicitly requested a force-build, prefer attempting
        # to download the repository source archive (tag or main branch) first
        # instead of fetching a prebuilt asset. This avoids downloading a
        # prebuilt binary only to then re-download the source archive.
        if [[ "${FORCE_BUILD:-0}" -eq 1 ]]; then
            if [[ -n "${RELEASE_TAG:-}" ]]; then
                REMOTE_URL="https://github.com/DiabloPower/burn2cool/archive/refs/tags/${RELEASE_TAG}.tar.gz"
            else
                REMOTE_URL="https://github.com/DiabloPower/burn2cool/archive/refs/heads/main.tar.gz"
            fi
            echo "➡️ --force-build requested; preferring source archive: $REMOTE_URL"
        else
            if auto_url=$(detect_release_asset) && [[ -n "$auto_url" ]]; then
                REMOTE_URL="$auto_url"
                echo "➡️ Auto-detected release asset: $REMOTE_URL"
            else
                read -r -p "Enter tarball/zip or binary URL (default: https://github.com/DiabloPower/burn2cool/archive/refs/heads/main.tar.gz): " REMOTE_URL
                REMOTE_URL=${REMOTE_URL:-https://github.com/DiabloPower/burn2cool/archive/refs/heads/main.tar.gz}
            fi
        fi
    fi
    echo "➡️ Downloading $REMOTE_URL"
    TMPDIR=$(mktemp -d)
    if command -v curl &>/dev/null; then
        curl -L --fail -o "$TMPDIR/archive" "$REMOTE_URL"
    elif command -v wget &>/dev/null; then
        wget -O "$TMPDIR/archive" "$REMOTE_URL"
    else
        echo "❌ Neither curl nor wget found; cannot download." >&2
        exit 1
    fi
    # Detect if the downloaded file is a direct binary (ELF) and skip extraction
    filedesc=$(file -b "$TMPDIR/archive" || true)
    filetype=$(file -b --mime-type "$TMPDIR/archive" || true)
    if echo "$filedesc" | grep -qi 'elf'; then
        echo "➡️ Detected ELF/binary download; will install directly from downloaded file"
        SRC_BIN_PATH="$TMPDIR/archive"
        SKIP_EXTRACTION=1
    else
        SKIP_EXTRACTION=0
    fi

    # If user explicitly requested a build, but the detected download was a
    # single ELF binary, attempt to fetch the source archive instead so we
    # can build from source. Prefer the release tag if it can be inferred,
    # otherwise fall back to the main branch tarball.
    if [[ "$FORCE_BUILD" -eq 1 && "$SKIP_EXTRACTION" -eq 1 ]]; then
        echo "➡️ --force-build requested but downloaded asset is a binary. Fetching source archive instead."
        # Try to infer release tag from REMOTE_URL if possible
        rel_tag=""
        if [[ -n "${REMOTE_URL:-}" && "$REMOTE_URL" =~ /releases/download/([^/]+)/ ]]; then
            rel_tag="${BASH_REMATCH[1]}"
        fi
        # Try tag-based archives, prefer common archive formats (.tar.gz then .zip)
        tried_any=0
        for ext in "tar.gz" "zip"; do
            if [[ -n "${rel_tag}" ]]; then
                try_url="https://github.com/DiabloPower/burn2cool/archive/refs/tags/${rel_tag}.${ext}"
            else
                try_url="https://github.com/DiabloPower/burn2cool/archive/refs/heads/main.${ext}"
            fi
            echo "➡️ Attempting to download source archive: $try_url"
            if command -v curl &>/dev/null; then
                if curl -L --fail -o "$TMPDIR/src_archive" "$try_url"; then
                    echo "➡️ Source archive downloaded to $TMPDIR/src_archive"
                    SKIP_EXTRACTION=0
                    TMP_ARCHIVE_PATH="$TMPDIR/src_archive"
                    filetype=$(file -b --mime-type "$TMP_ARCHIVE_PATH" || true)
                    # Try extraction: prefer tar, fallback to unzip
                    extraction_ok=0
                    if tar -tf "$TMP_ARCHIVE_PATH" >/dev/null 2>&1; then
                        if tar -xf "$TMP_ARCHIVE_PATH" -C "$TMPDIR" >/dev/null 2>&1; then
                            extraction_ok=1
                        else
                            echo "❌ tar extraction failed for $TMP_ARCHIVE_PATH"
                        fi
                    fi
                    if [[ $extraction_ok -ne 1 ]]; then
                        if command -v unzip >/dev/null 2>&1 && unzip -q "$TMP_ARCHIVE_PATH" -d "$TMPDIR" >/dev/null 2>&1; then
                            extraction_ok=1
                        fi
                    fi
                    if [[ $extraction_ok -eq 1 ]]; then
                        tried_any=1
                        break
                    else
                        echo "➡️ Extraction failed for $try_url; trying next candidate if available"
                        # continue to next ext
                    fi
                else
                    echo "➡️ Download failed for $try_url; trying next candidate"
                fi
            else
                echo "❌ curl not available to fetch source archive; cannot force-build from remote binary."
                break
            fi
        done
        if [[ $tried_any -ne 1 ]]; then
            echo "❌ Could not download & extract a suitable source archive for tag '${rel_tag:-main}'. Will fall back to current directory for build."
        fi
    fi

    # If we extracted a source archive into $TMPDIR above, try to locate the
    # expected SOURCE_FILE and update BUILD_DIR to point at the extracted source
    # so subsequent build steps operate on the downloaded sources rather than
    # the current working directory.
    if [[ -d "${TMPDIR:-}" ]]; then
        extracted_dir=$(find "$TMPDIR" -maxdepth 3 -type f -name "$SOURCE_FILE" -print0 | xargs -0 -r -n1 dirname | sed -n '1p' || true)
        if [[ -n "$extracted_dir" ]]; then
            BUILD_DIR="$extracted_dir"
            echo "✅ Found source in downloaded archive: $BUILD_DIR"
        fi
    fi

    # Extract (only if not an ELF/binary)
    if [[ "$SKIP_EXTRACTION" -ne 1 ]]; then
        echo "➡️ Extracting archive to $TMPDIR"
        filetype="$filetype"
    else
        echo "➡️ Skipping extraction for binary download"
    fi
    if [[ "$SKIP_EXTRACTION" -ne 1 ]]; then
    case "$filetype" in
        application/zip)
            unzip -q "$TMPDIR/archive" -d "$TMPDIR" || { echo "❌ unzip failed"; exit 1; }
            ;;
        application/x-gzip|application/gzip)
            tar -xzf "$TMPDIR/archive" -C "$TMPDIR" || { echo "❌ tar (gzip) extraction failed"; exit 1; }
            ;;
        application/x-bzip2)
            tar -xjf "$TMPDIR/archive" -C "$TMPDIR" || { echo "❌ tar (bzip2) extraction failed"; exit 1; }
            ;;
        application/x-xz)
            tar -xJf "$TMPDIR/archive" -C "$TMPDIR" || { echo "❌ tar (xz) extraction failed"; exit 1; }
            ;;
        application/x-tar)
            tar -xf "$TMPDIR/archive" -C "$TMPDIR" || { echo "❌ tar extraction failed"; exit 1; }
            ;;
        text/html*)
            echo "❌ Download looks like HTML (not an archive). The URL may have returned an HTML error/redirect page."
            echo "Saved file head (first 200 bytes):" && head -c 200 "$TMPDIR/archive" | sed -n '1,200p'
            exit 1
            ;;
        *)
            # Fallback: try tar, then unzip
            if tar -tf "$TMPDIR/archive" >/dev/null 2>&1; then
                tar -xf "$TMPDIR/archive" -C "$TMPDIR" || { echo "❌ tar extraction failed"; exit 1; }
            elif unzip -t "$TMPDIR/archive" >/dev/null 2>&1; then
                unzip -q "$TMPDIR/archive" -d "$TMPDIR" || { echo "❌ unzip failed"; exit 1; }
            else
                echo "❌ Unknown archive format: $filetype"; head -c 200 "$TMPDIR/archive" | sed -n '1,200p'; exit 1
            fi
            ;;
    esac
    fi

    # Try to find directory with SOURCE_FILE
    BUILD_DIR=$(find "$TMPDIR" -maxdepth 3 -type f -name "$SOURCE_FILE" -print0 | xargs -0 -r -n1 dirname | sed -n '1p' || true)
    if [[ -z "$BUILD_DIR" ]]; then
        echo "⚠️ Could not find $SOURCE_FILE in downloaded archive. Using current directory instead."
        BUILD_DIR="$(pwd)"
    else
        echo "✅ Found source in: $BUILD_DIR"
    fi
    
else
    BUILD_DIR="$(pwd)"
fi

# Detect prebuilt binaries in download (direct binary or archive containing executables)
SKIP_BUILD=false
if [[ -n "${TMPDIR:-}" && -f "$TMPDIR/archive" ]]; then
    # If the downloaded file itself is an ELF binary, install it directly
    if command -v file >/dev/null 2>&1 && file -b "$TMPDIR/archive" | grep -qi '\<elf\>'; then
        echo "✅ Detected downloaded ELF binary; will install directly from archive"
        SRC_BIN_PATH="$TMPDIR/archive"
        # try to detect ctl binary by name nearby (not applicable for single-file download)
        SKIP_BUILD=true
    fi
fi

if [[ "$SKIP_BUILD" != true ]]; then
    # Look for prebuilt binaries inside the extracted tree (common for release tarballs)
    PREBUILT_MAIN=$(find "$BUILD_DIR" -maxdepth 4 -type f -name "$BINARY_NAME" -print -quit || true)
    PREBUILT_CTL=$(find "$BUILD_DIR" -maxdepth 4 -type f -name "$CTL_BINARY_NAME" -print -quit || true)
    # if found and not a text source file, prefer it
    if [[ -n "$PREBUILT_MAIN" ]]; then
            echo "✅ Found prebuilt main binary in archive: $PREBUILT_MAIN"
            SRC_BIN_PATH="$PREBUILT_MAIN"
            # If the user explicitly requested a build, try to locate a source
            # archive from the release assets (or tag archive) instead of
            # accepting the prebuilt archive.
            if [[ "${FORCE_BUILD:-0}" -eq 1 ]]; then
                echo "➡️ --force-build requested but archive contains prebuilt binaries. Trying to find a source archive in release assets."
                # attempt to infer API URL for release assets
                rel_tag=""
                if [[ -n "${REMOTE_URL:-}" && "$REMOTE_URL" =~ /releases/download/([^/]+)/ ]]; then
                    rel_tag="${BASH_REMATCH[1]}"
                fi
                if [[ -z "${rel_tag:-}" ]]; then
                    api_url="https://api.github.com/repos/DiabloPower/burn2cool/releases/latest"
                else
                    api_url="https://api.github.com/repos/DiabloPower/burn2cool/releases/tags/${rel_tag}"
                fi
                if command -v curl >/dev/null 2>&1; then
                    # First, try tag/head archive endpoints directly (more likely to be actual source archives)
                    tried_src=0
                    for ext in "tar.gz" "zip"; do
                        if [[ -n "${rel_tag}" ]]; then
                            try_url="https://github.com/DiabloPower/burn2cool/archive/refs/tags/${rel_tag}.${ext}"
                        else
                            try_url="https://github.com/DiabloPower/burn2cool/archive/refs/heads/main.${ext}"
                        fi
                        echo "➡️ Attempting tag/head source archive: $try_url"
                        if curl -L --fail -o "$TMPDIR/src_archive" "$try_url"; then
                            echo "➡️ Downloaded candidate source archive to $TMPDIR/src_archive"
                            # try extraction (tar then unzip)
                            if tar -tf "$TMPDIR/src_archive" >/dev/null 2>&1 && tar -xf "$TMPDIR/src_archive" -C "$TMPDIR" >/dev/null 2>&1; then
                                extracted_dir=$(find "$TMPDIR" -maxdepth 3 -type f -name "$SOURCE_FILE" -print0 | xargs -0 -r -n1 dirname | sed -n '1p' || true)
                                if [[ -n "$extracted_dir" ]]; then
                                    BUILD_DIR="$extracted_dir"
                                    echo "✅ Found source in downloaded archive: $BUILD_DIR"
                                    SKIP_BUILD=false
                                    tried_src=1
                                    break
                                fi
                            elif command -v unzip >/dev/null 2>&1 && unzip -q "$TMPDIR/src_archive" -d "$TMPDIR" >/dev/null 2>&1; then
                                extracted_dir=$(find "$TMPDIR" -maxdepth 3 -type f -name "$SOURCE_FILE" -print0 | xargs -0 -r -n1 dirname | sed -n '1p' || true)
                                if [[ -n "$extracted_dir" ]]; then
                                    BUILD_DIR="$extracted_dir"
                                    echo "✅ Found source in downloaded archive: $BUILD_DIR"
                                    SKIP_BUILD=false
                                    tried_src=1
                                    break
                                fi
                            fi
                        fi
                    done
                    # If tag/head attempts failed, fall back to scanning release assets
                    if [[ "$tried_src" -ne 1 ]]; then
                        assets_list=$(curl -fsSL "$api_url" 2>/dev/null | sed -n 's/.*"browser_download_url": *"\([^\"]*\)".*/\1/p' || true)
                        # Prefer explicit repository archive endpoints or filenames indicating source
                        src_match=$(echo "$assets_list" | grep -iE '/archive/|source|src' | head -n1 || true)
                        # If none found, consider generic archives but exclude obvious binary/arch builds
                        if [[ -z "${src_match:-}" ]]; then
                            src_match=$(echo "$assets_list" | grep -iE '\.(tar\.gz|zip)$' | grep -viE 'linux|x86|amd64|arm|aarch64|win|windows|x86_64' | head -n1 || true)
                        fi
                        if [[ -n "$src_match" ]]; then
                            echo "➡️ Found candidate source archive: $src_match"
                            if curl -L --fail -o "$TMPDIR/src_archive" "$src_match"; then
                                echo "➡️ Downloaded source archive to $TMPDIR/src_archive"
                                # try extraction (tar then unzip)
                                if tar -tf "$TMPDIR/src_archive" >/dev/null 2>&1 && tar -xf "$TMPDIR/src_archive" -C "$TMPDIR" >/dev/null 2>&1; then
                                    extracted_dir=$(find "$TMPDIR" -maxdepth 3 -type f -name "$SOURCE_FILE" -print0 | xargs -0 -r -n1 dirname | sed -n '1p' || true)
                                    if [[ -n "$extracted_dir" ]]; then
                                        BUILD_DIR="$extracted_dir"
                                        echo "✅ Found source in downloaded archive: $BUILD_DIR"
                                        SKIP_BUILD=false
                                    fi
                                elif command -v unzip >/dev/null 2>&1 && unzip -q "$TMPDIR/src_archive" -d "$TMPDIR" >/dev/null 2>&1; then
                                    extracted_dir=$(find "$TMPDIR" -maxdepth 3 -type f -name "$SOURCE_FILE" -print0 | xargs -0 -r -n1 dirname | sed -n '1p' || true)
                                    if [[ -n "$extracted_dir" ]]; then
                                        BUILD_DIR="$extracted_dir"
                                        echo "✅ Found source in downloaded archive: $BUILD_DIR"
                                        SKIP_BUILD=false
                                    fi
                                else
                                    echo "➡️ Failed to extract candidate source archive; will fall back to prebuilt if no source found."
                                fi
                            else
                                echo "➡️ Failed to download candidate source archive: $src_match"
                            fi
                        else
                            echo "➡️ No source archive candidate found in release assets for tag ${rel_tag:-latest}."
                        fi
                    fi
                else
                    echo "➡️ curl not available to query release assets; cannot locate source archive."
                fi
                # If after attempting to find a source we still have no BUILD_DIR/source, fall back to prebuilt
                if [[ -z "${BUILD_DIR:-}" || ! -f "$BUILD_DIR/$SOURCE_FILE" ]]; then
                    echo "➡️ No source available; will use prebuilt binary from archive"
                    SKIP_BUILD=true
                fi
                # If the user requested --force-build, and after scanning tag/head
                # archives and release assets we still don't have source, abort
                # with guidance (do not silently fall back to prebuilt binaries).
                if [[ "${FORCE_BUILD:-0}" -eq 1 && "${SKIP_BUILD:-false}" == "true" ]]; then
                    echo "❌ --force-build requested but no source archive was found in release assets or tag/head endpoints."
                    echo "Options:"
                    echo "  1) Re-run without --force-build to install prebuilt binaries from the release."
                    echo "  2) Provide a source archive URL with --fetch-url <url> pointing to a repository archive or source tarball."
                    echo "  3) Clone the repository locally and run this installer from the repository root to build from source."
                    exit 1
                fi
            else
                SKIP_BUILD=true
            fi
    fi
    if [[ -n "$PREBUILT_CTL" ]]; then
        echo "✅ Found prebuilt control binary in archive: $PREBUILT_CTL"
        CTL_BIN_SRC="$PREBUILT_CTL"
    fi
    # Also detect prebuilt TUI binary in the archive
    PREBUILT_TUI=$(find "$BUILD_DIR" -maxdepth 4 -type f -name "$TUI_BIN_NAME" -print -quit || true)
    if [[ -n "$PREBUILT_TUI" ]]; then
        echo "✅ Found prebuilt TUI binary in archive: $PREBUILT_TUI"
        TUI_BIN_SRC="$PREBUILT_TUI"
    fi
fi

# If we have a downloaded single binary (SRC_BIN_PATH) and no ctl binary found,
# try to fetch the companion control binary from the same GitHub release (if applicable).
if [[ -n "${SRC_BIN_PATH:-}" && -z "${CTL_BIN_SRC:-}" && -n "${REMOTE_URL:-}" ]]; then
    if echo "$REMOTE_URL" | grep -q "/releases/download/"; then
        # try to extract tag from URL
        if [[ "$REMOTE_URL" =~ /releases/download/([^/]+)/ ]]; then
            rel_tag="${BASH_REMATCH[1]}"
        else
            rel_tag="${RELEASE_TAG:-}"
        fi
        if [[ -z "${rel_tag:-}" ]]; then
            api_url="https://api.github.com/repos/DiabloPower/burn2cool/releases/latest"
        else
            api_url="https://api.github.com/repos/DiabloPower/burn2cool/releases/tags/${rel_tag}"
        fi
        if command -v curl >/dev/null 2>&1; then
            assets=$(curl -fsSL "$api_url" 2>/dev/null | sed -n 's/.*"browser_download_url": *"\([^"]*\)".*/\1/p' || true)
            if [[ -n "$assets" ]]; then
                ctl_url=$(echo "$assets" | grep -i "/${CTL_BINARY_NAME}\(\.|$\)" | head -n1 || true)
                tui_url=$(echo "$assets" | grep -i "/${TUI_BIN_NAME}\(\.|$\)" | head -n1 || true)
                if [[ -n "$ctl_url" ]]; then
                    echo "➡️ Downloading companion control binary: $ctl_url"
                    if command -v curl >/dev/null 2>&1; then
                        curl -fSL -o "$TMPDIR/${CTL_BINARY_NAME}" "$ctl_url" || true
                        chmod +x "$TMPDIR/${CTL_BINARY_NAME}" || true
                        CTL_BIN_SRC="$TMPDIR/${CTL_BINARY_NAME}"
                    fi
                fi
                if [[ -n "$tui_url" ]]; then
                    echo "➡️ Downloading companion TUI binary: $tui_url"
                    if command -v curl >/dev/null 2>&1; then
                        curl -fSL -o "$TMPDIR/${TUI_BIN_NAME}" "$tui_url" || true
                        chmod +x "$TMPDIR/${TUI_BIN_NAME}" || true
                        TUI_BIN_SRC="$TMPDIR/${TUI_BIN_NAME}"
                    fi
                fi
            fi
        fi
    fi
fi

cd "$BUILD_DIR"
# Decide whether to use prebuilt binaries or build from source
ACTION=""
if [[ "$FORCE_BUILD" -eq 1 ]]; then
    ACTION="build"
elif [[ "$FORCE_BINARIES" -eq 1 ]]; then
    ACTION="install"
elif [[ -n "${SRC_BIN_PATH:-}" || -n "${PREBUILT_MAIN:-}" ]]; then
    # Prebuilt binaries detected
    if [[ "$AUTO_YES" -eq 1 ]]; then
        ACTION="install"
    else
        # Ask the user whether to install binaries or build from source
        read -r -p "Prebuilt binaries detected. Install binaries or build from source? [I/b] (default: Install): " USE_CHOICE
        USE_CHOICE=${USE_CHOICE:-I}
        if [[ "$USE_CHOICE" =~ ^[Bb] ]]; then
            ACTION="build"
        else
            ACTION="install"
        fi
    fi
fi

# If user chose to install binaries, skip dependency installation and build steps
if [[ "$ACTION" == "install" ]]; then
    SKIP_BUILD=true
elif [[ "$ACTION" == "build" ]]; then
    # User explicitly requested a build
    SKIP_BUILD=false
else
    # If --force-build was specified, ensure we do not skip building even
    # if an ELF/prebuilt binary was detected earlier.
    if [[ "$FORCE_BUILD" -eq 1 ]]; then
        SKIP_BUILD=false
    else
        SKIP_BUILD=${SKIP_BUILD:-false}
    fi
fi

# If the user selected to build but BUILD_DIR does not contain sources,
# attempt to fetch a source archive automatically (interactive) before
# trying to compile in the current directory. This mirrors the logic
# used when --force-build is given but applies when the user chose
# 'build' interactively after prebuilt binaries were detected.
if [[ "$ACTION" == "build" ]]; then
    if [[ ! -f "$BUILD_DIR/$SOURCE_FILE" ]]; then
        echo "⚠️ No source files found in $BUILD_DIR."
        if [[ "$AUTO_YES" -eq 1 ]]; then
            TRY_DL=Y
        else
            read -r -p "Attempt to download a source archive from the release (recommended)? [Y/n]: " TRY_DL
            TRY_DL=${TRY_DL:-Y}
        fi
        if [[ "$TRY_DL" =~ ^[Yy] ]]; then
            echo "➡️ Trying to locate a source archive for the release..."
            # Ensure TMPDIR exists for interactive download attempts
            if [[ -z "${TMPDIR:-}" || ! -d "${TMPDIR:-}" ]]; then
                TMPDIR=$(mktemp -d) || { echo "❌ Could not create temporary directory for downloads"; exit 1; }
            fi
            rel_tag=""
            if [[ -n "${REMOTE_URL:-}" && "$REMOTE_URL" =~ /releases/download/([^/]+)/ ]]; then
                rel_tag="${BASH_REMATCH[1]}"
            fi
            tried_src=0
            if command -v curl >/dev/null 2>&1; then
                for ext in "tar.gz" "zip"; do
                    if [[ -n "${rel_tag}" ]]; then
                        try_url="https://github.com/DiabloPower/burn2cool/archive/refs/tags/${rel_tag}.${ext}"
                    else
                        try_url="https://github.com/DiabloPower/burn2cool/archive/refs/heads/main.${ext}"
                    fi
                    echo "➡️ Attempting: $try_url"
                    if curl -L --fail -o "$TMPDIR/src_archive" "$try_url"; then
                        if tar -tf "$TMPDIR/src_archive" >/dev/null 2>&1 && tar -xf "$TMPDIR/src_archive" -C "$TMPDIR" >/dev/null 2>&1; then
                            extracted_dir=$(find "$TMPDIR" -maxdepth 3 -type f -name "$SOURCE_FILE" -print0 | xargs -0 -r -n1 dirname | sed -n '1p' || true)
                            if [[ -n "$extracted_dir" ]]; then
                                BUILD_DIR="$extracted_dir"
                                echo "✅ Found source in downloaded archive: $BUILD_DIR"
                                tried_src=1
                                break
                            fi
                        elif command -v unzip >/dev/null 2>&1 && unzip -q "$TMPDIR/src_archive" -d "$TMPDIR" >/dev/null 2>&1; then
                            extracted_dir=$(find "$TMPDIR" -maxdepth 3 -type f -name "$SOURCE_FILE" -print0 | xargs -0 -r -n1 dirname | sed -n '1p' || true)
                            if [[ -n "$extracted_dir" ]]; then
                                BUILD_DIR="$extracted_dir"
                                echo "✅ Found source in downloaded archive: $BUILD_DIR"
                                tried_src=1
                                break
                            fi
                        fi
                    fi
                done
                if [[ "$tried_src" -ne 1 ]]; then
                    if [[ -z "${rel_tag:-}" ]]; then
                        api_url="https://api.github.com/repos/DiabloPower/burn2cool/releases/latest"
                    else
                        api_url="https://api.github.com/repos/DiabloPower/burn2cool/releases/tags/${rel_tag}"
                    fi
                    assets_list=$(curl -fsSL "$api_url" 2>/dev/null | sed -n 's/.*"browser_download_url": *"\([^\"]*\)".*/\1/p' || true)
                    src_match=$(echo "$assets_list" | grep -iE '/archive/|source|src' | head -n1 || true)
                    if [[ -z "${src_match:-}" ]]; then
                        src_match=$(echo "$assets_list" | grep -iE '\.(tar\.gz|zip)$' | grep -viE 'linux|x86|amd64|arm|aarch64|win|windows|x86_64' | head -n1 || true)
                    fi
                    if [[ -n "$src_match" ]]; then
                        echo "➡️ Found candidate source archive: $src_match"
                        if curl -L --fail -o "$TMPDIR/src_archive" "$src_match"; then
                            if tar -tf "$TMPDIR/src_archive" >/dev/null 2>&1 && tar -xf "$TMPDIR/src_archive" -C "$TMPDIR" >/dev/null 2>&1; then
                                extracted_dir=$(find "$TMPDIR" -maxdepth 3 -type f -name "$SOURCE_FILE" -print0 | xargs -0 -r -n1 dirname | sed -n '1p' || true)
                                if [[ -n "$extracted_dir" ]]; then
                                    BUILD_DIR="$extracted_dir"
                                    echo "✅ Found source in downloaded archive: $BUILD_DIR"
                                    tried_src=1
                                fi
                            elif command -v unzip >/dev/null 2>&1 && unzip -q "$TMPDIR/src_archive" -d "$TMPDIR" >/dev/null 2>&1; then
                                extracted_dir=$(find "$TMPDIR" -maxdepth 3 -type f -name "$SOURCE_FILE" -print0 | xargs -0 -r -n1 dirname | sed -n '1p' || true)
                                if [[ -n "$extracted_dir" ]]; then
                                    BUILD_DIR="$extracted_dir"
                                    echo "✅ Found source in downloaded archive: $BUILD_DIR"
                                    tried_src=1
                                fi
                            else
                                echo "➡️ Failed to extract candidate source archive"
                            fi
                        else
                            echo "➡️ Failed to download candidate source archive: $src_match"
                        fi
                    else
                        echo "➡️ No source archive candidates found in release assets."
                    fi
                fi
            else
                echo "➡️ curl not available; cannot attempt remote source download."
            fi
            if [[ ! -f "$BUILD_DIR/$SOURCE_FILE" ]]; then
                if [[ "$AUTO_YES" -eq 1 ]]; then
                    echo "❌ No source found; aborting (non-interactive)."
                    exit 1
                else
                    read -r -p "No source found. Continue and attempt build in $BUILD_DIR anyway? [y/N]: " CONTINUE_BLDR
                    CONTINUE_BLDR=${CONTINUE_BLDR:-N}
                    if [[ ! "$CONTINUE_BLDR" =~ ^[Yy] ]]; then
                        echo "Aborting install. Consider running with --fetch-url or cloning the repo locally to build from source."; exit 1
                    fi
                fi
            fi
        else
            echo "Aborting: user declined to attempt remote source download. You can re-run with --fetch-url or clone repo locally."; exit 1
        fi
    fi
fi

# --- Offer to install build dependencies and build (skipped if prebuilt selected) ---
if [[ "$SKIP_BUILD" != true ]]; then
    printf "\n🔧 Build dependencies required: gcc, make, pkg-config, python3, curl, git\n"
    if [[ "$ENABLE_TUI" -eq 1 ]]; then
        echo " (TUI selected: will also require libncurses-dev/ncurses-devel)"
    else
        echo
    fi

    # detect package manager now (so this message doesn't appear before the welcome banner)
    PKG_MANAGER=$(detect_package_manager)

    if [[ "$AUTO_YES" -eq 1 ]]; then
        INSTALL_DEPS=Y
    else
        if [[ -n "$PKG_MANAGER" ]]; then
            read -r -p "Install missing dependencies now? (will use $PKG_MANAGER) [Y/n]: " INSTALL_DEPS
            INSTALL_DEPS=${INSTALL_DEPS:-Y}
        else
            read -r -p "Proceed to build without installing dependencies? (you may need to install them manually) [y/N]: " INSTALL_DEPS
            INSTALL_DEPS=${INSTALL_DEPS:-N}
        fi
    fi

    if [[ "$INSTALL_DEPS" =~ ^[Yy] ]]; then
        case "$PKG_MANAGER" in
            apt-get)
                desired=(build-essential pkg-config python3 python3-pip curl git)
                if [[ "$ENABLE_TUI" -eq 1 ]] && ! has_ncurses; then
                    desired+=(libncurses-dev)
                fi
                ;;
            dnf)
                sudo dnf groupinstall -y "Development Tools" || true
                desired=(pkgconf-pkg-config python3 python3-pip curl git)
                if [[ "$ENABLE_TUI" -eq 1 ]] && ! has_ncurses; then
                    desired+=(ncurses-devel)
                fi
                ;;
            yum)
                sudo yum groupinstall -y "Development Tools" || true
                desired=(pkgconf-pkg-config python3 python3-pip curl git)
                if [[ "$ENABLE_TUI" -eq 1 ]] && ! has_ncurses; then
                    desired+=(ncurses-devel)
                fi
                ;;
            pacman)
                desired=(base-devel pkgconf python python-pip curl git)
                if [[ "$ENABLE_TUI" -eq 1 ]] && ! has_ncurses; then
                    desired+=(ncurses)
                fi
                ;;
            zypper)
                sudo zypper install -y -t pattern devel_C_C++ || true
                desired=(pkg-config python3 python3-pip curl git)
                if [[ "$ENABLE_TUI" -eq 1 ]] && ! has_ncurses; then
                    desired+=(ncurses-devel)
                fi
                ;;
            apk)
                desired=(build-base pkgconfig python3 py3-pip curl git)
                if [[ "$ENABLE_TUI" -eq 1 ]] && ! has_ncurses; then
                    desired+=(ncurses-dev)
                fi
                ;;
            *)
                echo "Skipping automated dependency installation." ;;
        esac

        # Install only missing packages
        if [[ ${#desired[@]} -gt 0 ]]; then
            missing=()
            for p in "${desired[@]}"; do
                if ! has_package "$p"; then
                    missing+=("$p")
                else
                    echo "ℹ️ Package '$p' already installed; skipping."
                fi
            done
            if [[ ${#missing[@]} -gt 0 ]]; then
                echo "➡️ Installing missing packages: ${missing[*]}"
                install_package "${missing[@]}"
            else
                echo "✅ All required packages are already installed."
            fi
        fi
    fi

    # --- Build ---
    # Ensure BUILD_DIR points to the directory that actually contains
    # the expected SOURCE_FILE. Some archives extract with an extra
    # nesting level, so re-scan recursively and use the first match.
    # Prefer scanning the temporary extraction location first (archives
    # are extracted into $TMPDIR). If that fails, fall back to scanning
    # the current BUILD_DIR recursively.
    srcpath=""
    if [[ -n "${TMPDIR:-}" && -d "$TMPDIR" ]]; then
        srcpath=$(find "$TMPDIR" -type f -name "$SOURCE_FILE" -print -quit || true)
    fi
    if [[ -z "$srcpath" ]]; then
        srcpath=$(find "$BUILD_DIR" -type f -name "$SOURCE_FILE" -print -quit || true)
    fi
    if [[ -n "$srcpath" ]]; then
        BUILD_DIR=$(dirname "$srcpath")
    fi
    printf "\n🛠️ Building project in: %s\n" "$BUILD_DIR"
    if [[ -f Makefile || -f makefile ]]; then
        echo "➡️ Running 'make assets' then 'make' if available"
        if make assets 2>/dev/null || true; then
            echo "make assets executed (if present)"
        fi
        if make -j"$(nproc)" 2>/dev/null; then
            echo "✅ make succeeded"
            # If make didn't produce the expected binary, fall back to direct compile
            if ! find "$BUILD_DIR" -maxdepth 2 -type f -name "$BINARY_NAME" -print -quit >/dev/null 2>&1; then
                echo "⚠️ 'make' did not produce $BINARY_NAME; falling back to direct compile"
                BUILD_FALLBACK=true
            fi
        else
            echo "⚠️ make failed or not available; falling back to direct compile"
            BUILD_FALLBACK=true
        fi
    else
        BUILD_FALLBACK=true
    fi

    if [[ ${BUILD_FALLBACK:-false} == true ]]; then
        echo "➡️ Compiling sources directly"
        # If the main source file is not present at the top-level of
        # BUILD_DIR, try to find it recursively (some archives place
        # sources inside a nested subdirectory). If found, switch to
        # that directory before compiling.
        if [[ ! -f "$SOURCE_FILE" ]]; then
            nested_src=$(find . -type f -name "$SOURCE_FILE" -print -quit || true)
            if [[ -n "$nested_src" ]]; then
                echo "➡️ Found $SOURCE_FILE at nested path: $nested_src"
                nested_dir=$(dirname "$nested_src")
                echo "➡️ Changing directory to $nested_dir to compile"
                cd "$nested_dir"
            fi
        fi

        if [[ -f "$SOURCE_FILE" ]]; then
            # Link ncurses only if TUI is requested or code requires it; safe to add if lib exists
            TUI_LDFLAGS=""
            if [[ "$ENABLE_TUI" -eq 1 ]]; then
                TUI_LDFLAGS="-lncurses"
            fi
            gcc -O2 -Wall -Wextra -std=c11 -pthread -o "$BINARY_NAME" "$SOURCE_FILE" $TUI_LDFLAGS || { echo "❌ Failed to compile $SOURCE_FILE"; exit 1; }
        else
            echo "❌ $SOURCE_FILE not found in $BUILD_DIR"; exit 1
        fi
        if [[ -f "$CTL_SOURCE_FILE" ]]; then
            gcc -O2 -Wall -Wextra -std=c11 -o "$CTL_BINARY_NAME" "$CTL_SOURCE_FILE" || echo "⚠️ Control utility not compiled (source missing)"
        fi
    fi

    # If make produced binaries in subdir (e.g., bin/), try to locate them
    if [[ -f "$BINARY_NAME" ]]; then
        # If compilation happened in a nested directory (we may have cd'd into it),
        # prefer the binary in the current working directory so we don't assume
        # it's at the top-level $BUILD_DIR.
        SRC_BIN_PATH="$(pwd)/$BINARY_NAME"
    else
        # Allow a slightly deeper search for build outputs produced in subdirs
        SRC_BIN_PATH=$(find "$BUILD_DIR" -maxdepth 4 -type f -name "$BINARY_NAME" -print -quit || true)
    fi
    if [[ -z "$SRC_BIN_PATH" ]]; then
        echo "❌ Could not find built binary $BINARY_NAME"; exit 1
    fi
else
    echo "➡️ SKIP_BUILD=true; skipping dependency installation and build steps (using prebuilt binary)."
fi

# If the user chose to build from source, prefer the freshly-built binary
# over any previously-downloaded prebuilt artifact. This ensures that when
# the user selects 'build' we install what we just compiled.
if [[ "${ACTION:-}" == "build" ]]; then
    # If compiled into current dir, prefer that
    if [[ -f "$BINARY_NAME" ]]; then
        SRC_BIN_PATH="$(pwd)/$BINARY_NAME"
    else
        # Try to find the binary under BUILD_DIR first (deeper search)
        SRC_BIN_PATH=$(find "${BUILD_DIR:-.}" -maxdepth 6 -type f -name "$BINARY_NAME" -print -quit || true)
    fi
fi

# Before installing, ensure SRC_BIN_PATH points to an actual file. Some
# build flows `cd` into a nested dir which can make earlier path resolution
# incorrect; try a few likely locations to find the built binary.
if [[ -n "${SRC_BIN_PATH:-}" && ! -f "$SRC_BIN_PATH" ]]; then
    echo "⚠️ Expected built binary not found at $SRC_BIN_PATH; attempting to resolve actual path..."
    # Search current directory first, then TMPDIR (extraction), then BUILD_DIR
    candidate=$(find . -type f -name "$BINARY_NAME" -print -quit || true)
    if [[ -z "$candidate" && -n "${TMPDIR:-}" ]]; then
        candidate=$(find "$TMPDIR" -type f -name "$BINARY_NAME" -print -quit || true)
    fi
    if [[ -z "$candidate" && -n "${BUILD_DIR:-}" ]]; then
        candidate=$(find "$BUILD_DIR" -maxdepth 6 -type f -name "$BINARY_NAME" -print -quit || true)
    fi
    if [[ -n "$candidate" ]]; then
        SRC_BIN_PATH="$(readlink -f "$candidate")"
        echo "➡️ Resolved built binary: $SRC_BIN_PATH"
    else
        echo "❌ Could not locate built binary $BINARY_NAME. pwd=$(pwd) BUILD_DIR=$BUILD_DIR TMPDIR=${TMPDIR:-}" >&2
        echo "Try re-running with --fetch-url or inspect the extracted archive." >&2
        exit 1
    fi
fi

echo "➡️ Installing binaries to /usr/local/bin (sudo required)"

# Helper: attempt to install a file, stopping service if it blocks (Text file busy)
install_binary() {
    local src="$1" dst="$2"
    # Try a normal copy first
    if sudo cp "$src" "$dst" 2>/dev/null; then
        sudo chmod +x "$dst" || true
        return 0
    fi

    echo "⚠️ Could not overwrite $dst (maybe it's running). Attempting safe replace..."

    # If a systemd service exists for the binary, stop it first
    if command -v systemctl &>/dev/null; then
        if systemctl is-active --quiet "$BINARY_NAME".service 2>/dev/null; then
            echo "Stopping service $BINARY_NAME to replace binary..."
            sudo systemctl stop "$BINARY_NAME".service || true
        fi
    fi

    # Try again
    if sudo cp "$src" "$dst"; then
        sudo chmod +x "$dst" || true
        return 0
    fi

    echo "❌ Failed to install $dst after stopping service." >&2
    return 1
}

if ! install_binary "$SRC_BIN_PATH" "$BUILD_PATH"; then
    echo "❌ Could not install main binary to $BUILD_PATH"; exit 1
fi

# Ensure CTL_BIN_SRC points to a control-binary path if it exists (from archive, download, or build)
if [[ -z "${CTL_BIN_SRC:-}" ]]; then
    # check current build dir (we cd'd there earlier)
    CTL_BIN_SRC=$(find "$BUILD_DIR" -maxdepth 2 -type f -name "$CTL_BINARY_NAME" -print -quit || true)
fi
if [[ -n "${CTL_BIN_SRC:-}" && -f "$CTL_BIN_SRC" ]]; then
    install_binary "$CTL_BIN_SRC" "$CTL_BUILD_PATH" || true
fi

# Install prebuilt TUI if requested or if ENABLE_TUI build produced one
if [[ -n "${TUI_BIN_SRC:-}" && -f "$TUI_BIN_SRC" ]]; then
    # If user explicitly asked to install TUI or auto-yes, or if they selected install and prebuilt exists
    if [[ "$INSTALL_TUI" -eq 1 || "$AUTO_YES" -eq 1 || "$SKIP_BUILD" == true ]]; then
        echo "➡️ Installing TUI binary to $TUI_BUILD_PATH"
        install_binary "$TUI_BIN_SRC" "$TUI_BUILD_PATH" || true
    else
        read -r -p "Install companion TUI binary ($TUI_BIN_NAME)? [y/N]: " INSTALL_TUI_ANS
        INSTALL_TUI_ANS=${INSTALL_TUI_ANS:-N}
        if [[ "$INSTALL_TUI_ANS" =~ ^[Yy] ]]; then
            install_binary "$TUI_BIN_SRC" "$TUI_BUILD_PATH" || true
        fi
    fi
fi

# Optional: build/install TUI if requested
if [[ "$ENABLE_TUI" -eq 1 ]]; then
    printf "\n🖥️  Building/installing ncurses TUI (if source present)\n"
    TUI_BIN_NAME="cpu_throttle_tui"
    TUI_SRC1="cpu_throttle_tui.c"
    TUI_SRC2="tui.c"
    if [[ -f "$TUI_SRC1" ]]; then
        gcc -O2 -Wall -Wextra -std=c11 -o "$TUI_BIN_NAME" "$TUI_SRC1" -lncurses || echo "⚠️ Failed to build TUI from $TUI_SRC1"
    elif [[ -f "$TUI_SRC2" ]]; then
        gcc -O2 -Wall -Wextra -std=c11 -o "$TUI_BIN_NAME" "$TUI_SRC2" -lncurses || echo "⚠️ Failed to build TUI from $TUI_SRC2"
    else
        # maybe make built it
        TUI_BIN_PATH=$(find "$BUILD_DIR" -maxdepth 2 -type f -name "$TUI_BIN_NAME" -print -quit || true)
        if [[ -n "$TUI_BIN_PATH" ]]; then
            cp "$TUI_BIN_PATH" "$TUI_BIN_NAME" || true
        else
            echo "ℹ️ No TUI source or binary found; skipping TUI build."
            TUI_BIN_NAME=""
        fi
    fi
    if [[ -n "$TUI_BIN_NAME" && -f "$TUI_BIN_NAME" ]]; then
        sudo cp "$TUI_BIN_NAME" "/usr/local/bin/$TUI_BIN_NAME" || true
        sudo chmod +x "/usr/local/bin/$TUI_BIN_NAME" || true
        echo "✅ Installed TUI: /usr/local/bin/$TUI_BIN_NAME"
    fi
fi

echo "✅ Installed: $BUILD_PATH"

# --- Web API / firewall / service ---
if [[ "$AUTO_YES" -eq 1 ]]; then
    ENABLE_WEB=Y
else
    read -r -p "Enable Web API (embedded dashboard) by default and open firewall port 8086? [y/N]: " ENABLE_WEB
    ENABLE_WEB=${ENABLE_WEB:-N}
fi

port_in_use() {
    local p="$1"
    if command -v ss &>/dev/null; then
        ss -ltn 2>/dev/null | awk '{print $4}' | sed -E 's/.*://g' | grep -x -- "$p" >/dev/null 2>&1 && return 0 || return 1
    elif command -v netstat &>/dev/null; then
        netstat -tln 2>/dev/null | awk '{print $4}' | sed -E 's/.*://g' | grep -x -- "$p" >/dev/null 2>&1 && return 0 || return 1
    else
        # Cannot reliably detect; assume free
        return 1
    fi
}

choose_available_port() {
    local preferred=(8086 8286 8386 8486)
    for p in "${preferred[@]}"; do
        if ! port_in_use "$p"; then
            echo "$p"
            return 0
        fi
    done
    # none found; ask user
    read -r -p "No preferred ports free. Enter custom port to use for web API: " custom
    echo "$custom"
}

if [[ "$ENABLE_WEB" =~ ^[Yy] ]]; then
    # If user passed --port, use it (but warn if in use)
    if [[ -n "$OVERRIDE_PORT" ]]; then
        WEB_PORT="$OVERRIDE_PORT"
        if port_in_use "$WEB_PORT"; then
            echo "⚠️ Requested port $WEB_PORT appears to be in use. Aborting web API enablement.";
            WEB_PORT=""
        fi
    else
        # pick a port (prefer 8086) but ensure it's free
        if port_in_use 8086; then
            echo "⚠️ Port 8086 appears to be in use. Selecting an alternative..."
            WEB_PORT=$(choose_available_port)
            if [[ -z "$WEB_PORT" ]]; then
                echo "❌ No port selected. Aborting web API enablement.";
            else
                if [[ "$AUTO_YES" -eq 1 ]]; then
                    USE_ALT=Y
                else
                    read -r -p "Use port ${WEB_PORT} for the Web API? [Y/n]: " USE_ALT
                    USE_ALT=${USE_ALT:-Y}
                fi
                if [[ ! "$USE_ALT" =~ ^[Yy] ]]; then
                    if [[ "$AUTO_YES" -eq 1 ]]; then
                        echo "Auto-yes enabled; using ${WEB_PORT}"
                    else
                        read -r -p "Enter desired port for Web API: " WEB_PORT
                    fi
                fi
            fi
        else
            WEB_PORT=8086
        fi
    fi

    if [[ -n "${WEB_PORT:-}" ]]; then
        echo "➡️ Enabling web API in config (/etc/cpu_throttle.conf) with port ${WEB_PORT}"
        sudo mkdir -p /etc
        if sudo grep -q "^web_port=" /etc/cpu_throttle.conf 2>/dev/null; then
            sudo sed -i "s/^web_port=.*/web_port=${WEB_PORT}/" /etc/cpu_throttle.conf || true
        else
            echo "web_port=${WEB_PORT}" | sudo tee -a /etc/cpu_throttle.conf > /dev/null
        fi

        # Firewall rules
        if command -v ufw &>/dev/null; then
            if [[ "$AUTO_YES" -eq 1 ]]; then
                UFW_ANS=Y
            else
                read -r -p "Detected ufw. Add rule to allow ${WEB_PORT}/tcp? [Y/n]: " UFW_ANS
                UFW_ANS=${UFW_ANS:-Y}
            fi
            if [[ "$UFW_ANS" =~ ^[Yy] ]]; then
                sudo ufw allow ${WEB_PORT}/tcp || true
            fi
        fi
        if command -v firewall-cmd &>/dev/null; then
            if [[ "$AUTO_YES" -eq 1 ]]; then
                FWD_ANS=Y
            else
                read -r -p "Detected firewalld. Add permanent rule for ${WEB_PORT}/tcp? [Y/n]: " FWD_ANS
                FWD_ANS=${FWD_ANS:-Y}
            fi
            if [[ "$FWD_ANS" =~ ^[Yy] ]]; then
                sudo firewall-cmd --permanent --add-port=${WEB_PORT}/tcp || true
                sudo firewall-cmd --reload || true
            fi
        fi
        if ! command -v ufw &>/dev/null && ! command -v firewall-cmd &>/dev/null; then
            echo "ℹ️ No ufw/firewalld detected. Please open port ${WEB_PORT}/tcp in your firewall if needed."
        fi
    fi
fi

# Prepare systemd service
SERVICE_EXISTS=false
if systemctl list-unit-files | grep -q "^${BINARY_NAME}\.service"; then
    SERVICE_EXISTS=true
    echo "⚠️  Service ${BINARY_NAME} already exists; will back it up before replacing."
    if [[ -f "$SERVICE_PATH" ]]; then
        sudo cp "$SERVICE_PATH" "${SERVICE_PATH}.backup.$(date +%Y%m%d_%H%M%S)" || true
    fi
fi

cat <<EOF | sudo tee "$SERVICE_PATH" > /dev/null
[Unit]
Description=cpu_throttle daemon (temperature-based CPU governor)
After=network.target

[Service]
Type=simple
ExecStart=$BUILD_PATH
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload || true
sudo systemctl enable --now "$BINARY_NAME".service || true

if [[ "$SERVICE_EXISTS" == true ]]; then
    echo "🔄 Replaced existing service $BINARY_NAME"
fi

if systemctl is-active --quiet "$BINARY_NAME".service; then
    echo "✅ Service $BINARY_NAME enabled and running"
else
    echo "⚠️ Service $BINARY_NAME not running; check 'sudo journalctl -u $BINARY_NAME -n 50'"
fi

printf "\n🎉 Installation complete.\n"
echo " - Binary: $BUILD_PATH"
echo " - Service: $SERVICE_PATH"
echo "To control the daemon: sudo $CTL_BUILD_PATH <command> (if installed)"
