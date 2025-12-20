# Burn2Cool — The ROG Tamer CPU Throttle Daemon

<img width="1100" height="980" alt="image" src="https://github.com/user-attachments/assets/08bc5cde-f9e2-42d5-8ac4-413b858f25b3" />

A small, efficient Linux daemon that dynamically adjusts CPU maximum frequency based on temperature. It provides runtime control, profile management, an embedded web UI (optional) and an ncurses TUI (optional).

## ✨ What's New in v4.1

- **Automatic Sensor Detection (HWMon preferred)**: Smart auto-detection of the best CPU temperature sensor — prefers HWMon sensors (`/sys/class/hwmon/...`) when available and falls back to thermal zones (`/sys/class/thermal/...`) with manual overrides available
- **Average Temperature Support**: Use average temperature across multiple CPU zones for better accuracy
- **System Skins**: Install and manage custom web UI skins system-wide
- **Enhanced Security**: Hardened installation with secure command execution
- **Improved Hysteresis**: Smoother frequency transitions with oscillation prevention

For detailed usage, examples and the full reference documentation, see the project wiki:

https://github.com/DiabloPower/burn2cool/wiki

Quick start
-----------

1) Run a prebuilt binary (recommended for most users)

```bash
# download the desired release from Releases and run it
wget -O cpu_throttle https://github.com/DiabloPower/burn2cool/releases/download/<tag>/cpu_throttle
chmod +x cpu_throttle
sudo ./cpu_throttle
```

2) Install via the installer (detects distro, builds if needed, creates systemd unit)

```bash
# non-interactive (installs everything: daemon, ctl, tui, gui, web API, service)
curl -fsSL https://raw.githubusercontent.com/DiabloPower/burn2cool/main/install_cpu-throttle.sh | bash -s -- --yes
```

```bash
# or download and run locally
wget https://raw.githubusercontent.com/DiabloPower/burn2cool/main/install_cpu-throttle.sh
bash install_cpu-throttle.sh --yes
```

3) Build from source (developers)

```bash
make assets      # generate include/*.h from assets/
make -j$(nproc)
./cpu_throttle --web-port 8086
```

Key notes
---------
- The web dashboard and REST API are embedded in the daemon (default port 8086). Configure via `/etc/cpu_throttle.conf` or `--web-port`.
- **New in v4.1**: Automatic sensor detection prefers HWMon sensors when available and falls back to thermal zones; the Web UI shows the active sensor path and source (HWMon vs Thermal).
- **New in v4.0**: Average temperature mode uses multiple CPU zones for more accurate readings.
- **New in v4.0**: System-wide skins can be installed and managed through the web UI.
- `make assets` converts files in `assets/` into `include/*.h` (the Makefile falls back to a Python generator if `xxd` is missing).
- Runtime control is available via the Unix socket (default `/tmp/cpu_throttle.sock`) using the `cpu_throttle_ctl` helper.
	- The `cpu_throttle_ctl` control helper also supports `get-excluded-types`, `toggle-excluded <token>`, and `set-excluded-types --merge|--remove` to manage excluded thermal types without clobbering global settings.

Where to find more
-------------------
- Wiki (full docs, examples, installer flags): https://github.com/DiabloPower/burn2cool/wiki
- Releases (prebuilt binaries): https://github.com/DiabloPower/burn2cool/releases
- Security: https://github.com/DiabloPower/burn2cool/blob/main/SECURITY.md

Files in this repo
------------------
- `cpu_throttle.c` — daemon source
- `cpu_throttle_ctl.c` — control utility source  
- `cpu_throttle_tui.c` — optional ncurses TUI source
- `gui_tray/src/` — GUI tray application source
- `install_cpu-throttle.sh` — installer script (builds/installs and sets up service); supports `--install-skin <archive>` to install a system skin and activate it
- `Makefile`, `assets/`, `include/`

---

## 📊 Temperature Thresholds

Default behavior (temp_max = 95°C):
- **< 65°C**: 100% performance (full speed)
- **65°C to 95°C**: Linear throttling from 100% to 50% performance
- **≥ 95°C**: Minimum frequency (emergency mode)

> 💡 **New in v4.0**: You can now use `--avg-temp` for average temperature across multiple CPU zones. The CLI flag `--sensor-source <auto|hwmon|thermal>` allows you to prefer a sensor source (HWMon / thermal), and explicit sensor path selection is still available with `--sensor <path>`. By default the daemon tries to auto-detect the best sensor — it prefers HWMon sensors (e.g. `coretemp`, `k10temp`) when present and falls back to thermal zones.

> 💡 Thresholds scale proportionally when using `--temp-max`. For example, `--temp-max 85` adjusts all thresholds accordingly (throttling starts at 55°C and reaches minimum at 85°C).

---

## 🛠️ Build from Source

### Dependencies

The install script automatically detects your distribution and installs required dependencies:

- **Debian/Ubuntu**: `build-essential`
- **Fedora/RHEL/CentOS**: `Development Tools` + `glibc-devel`
- **Arch Linux**: `base-devel`
- **openSUSE**: `devel_basis` + `glibc-devel`
- **Alpine Linux**: `build-base`

Manual installation (Debian/Ubuntu):
```bash
sudo apt update
sudo apt install build-essential gcc libc6-dev
```

### Compile

```bash
gcc -o cpu_throttle cpu_throttle.c -Wall
gcc -o cpu_throttle_ctl cpu_throttle_ctl.c -Wall
```

> 💡 The `install_cpu-throttle.sh` script handles compilation, dependency installation, and service setup automatically across all major distributions.

---

## ⚙️ Command Line Options

```
--dry-run              Simulation mode (no actual frequency changes)
--log <path>           Append log messages to a file (e.g., /var/log/cpu_throttle.log)
--sensor [path|list]   Manually specify temp sensor file (path) or use `list` to enumerate available sensors
                       (default: auto-detect; daemon prefers HWMon and falls back to thermal zones)
--sensor-source <auto|hwmon|thermal>
                       Prefer a source type when auto-detecting sensors (default: auto)
--thermal-zone <num>   Specify thermal zone number (overrides auto-detection)
--avg-temp             Use average temperature across CPU-related thermal zones
--safe-min <freq>      Minimum frequency limit in kHz (e.g. 2000000)
--safe-max <freq>      Maximum frequency limit in kHz (e.g. 3500000)
--temp-max <temp>      Maximum temperature threshold in °C (default: 95, range: 50-110)
--web-port [port]      Start the web UI on the given port (use default if omitted)
--verbose              Enable verbose logging
--quiet                Quiet mode (errors only)
--silent               Silent mode (no output)
--test                 Run unit tests and exit
--help                 Show help message
--install-skin <file>  Install a skin archive (tar.gz or zip) and activate it system-wide

Quick CLI tips:
- Use `cpu_throttle_ctl get-excluded-types` to inspect current excluded types without overwriting.
- `cpu_throttle_ctl toggle-excluded wifi` toggles a token (substring matching by default).
- `cpu_throttle_ctl set-excluded-types --merge int3400,wifi` merges tokens into the current list; `--remove` supports removal and `--exact` restricts to exact matches.
```

> 💡 For complete CLI reference and configuration options, see the [Configuration Wiki](https://github.com/DiabloPower/burn2cool/wiki/Configuration).

---

## 🧠 Technical Details

### Socket Communication
The daemon listens on `/tmp/cpu_throttle.sock` for runtime control commands. The `cpu_throttle_ctl` utility communicates with this socket to adjust settings without requiring a daemon restart.

### Hysteresis
The daemon uses linear scaling with hysteresis to prevent frequency oscillation. Frequency changes occur smoothly when temperature thresholds are crossed, with a default 3°C hysteresis buffer to avoid rapid switching.

### Signal Handling
Graceful shutdown on SIGINT (Ctrl+C) and SIGTERM, ensuring proper cleanup of the control socket.
