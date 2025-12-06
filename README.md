# Burn2Cool — CPU Throttle Daemon

A small, efficient Linux daemon that dynamically adjusts CPU maximum frequency based on temperature. It provides runtime control, profile management, an embedded web UI (optional) and an ncurses TUI (optional).

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
# non-interactive
bash -c "$(curl -fsSL https://raw.githubusercontent.com/DiabloPower/burn2cool/main/install_cpu-throttle.sh)" -- -y
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
- `make assets` converts files in `assets/` into `include/*.h` (the Makefile falls back to a Python generator if `xxd` is missing).
- Runtime control is available via the Unix socket (default `/tmp/cpu_throttle.sock`) using the `cpu_throttle_ctl` helper.

Where to find more
-------------------
- Wiki (full docs, examples, installer flags): https://github.com/DiabloPower/burn2cool/wiki
- Releases (prebuilt binaries): https://github.com/DiabloPower/burn2cool/releases

Files in this repo
------------------
- `cpu_throttle.c` — daemon source
- `cpu_throttle_ctl.c` — control utility source
- `cpu_throttle_tui.c` — optional ncurses TUI source
- `install_cpu-throttle.sh` — installer script (builds/installs and sets up service)
- `Makefile`, `assets/`, `include/`

If you want, I can now apply a more targeted edit (e.g., shorten sections, add usage examples, or sync the README's CLI snippets with `--help`). Should I commit this README update now?

---

## 📊 Temperature Thresholds

Default behavior (temp_max = 95°C):
- **< 75°C**: 100% performance (full speed)
- **75°C**: 85% performance (light throttling)
- **82°C**: 65% performance (medium throttling)
- **88°C**: 40% performance (strong throttling)
- **≥ 95°C**: Minimum frequency (emergency mode)

> 💡 Thresholds scale proportionally when using `--temp-max`. For example, `--temp-max 85` adjusts all thresholds accordingly.

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
--log <path>           Log file path for debugging
--sensor <path>        Custom temperature sensor path (default: /sys/class/thermal/thermal_zone0/temp)
--safe-min <freq>      Minimum frequency limit in kHz
--safe-max <freq>      Maximum frequency limit in kHz
--temp-max <temp>      Maximum temperature threshold in °C (default: 95, range: 50-110)
--help                 Show help message
```

---

## 🧠 Technical Details

### Socket Communication
The daemon listens on `/tmp/cpu_throttle.sock` for runtime control commands. The `cpu_throttle_ctl` utility communicates with this socket to adjust settings without requiring a daemon restart.

### Hysteresis
The daemon uses 10% hysteresis to prevent frequency oscillation. Frequency changes only occur when the calculated target differs significantly from the current frequency.

### Signal Handling
Graceful shutdown on SIGINT (Ctrl+C) and SIGTERM, ensuring proper cleanup of the control socket.
