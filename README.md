# Burn2Cool — The ROG Tamer CPU Throttle Daemon

A lightweight Linux service that dynamically throttles CPU frequency based on temperature. Originally designed to tame the powerful (and sometimes thermally confused) **ASUS ROG Strix Hero III**, it works on most Linux systems with standard thermal zones and CPUFreq drivers.

---

## 🔧 What's Inside

### 1. `cpu_throttle` (C program)
- **Temperature-based throttling** with adjustable thresholds optimized for mobile CPUs
- **Runtime control** via Unix Socket - change settings on-the-fly without restarting!
- **Safe-Min/Safe-Max** for frequency limits - perfect for switching between gaming and power-saving
- Dynamically adjusts CPU max frequency with intelligent hysteresis
- Uses min/max freq from sysfs (`cpuinfo_min_freq`, `cpuinfo_max_freq`)
- Fast and efficient: suitable for long-term background use
- Optional flags: `--dry-run`, `--log`, `--sensor`, `--safe-min`, `--safe-max`, `--temp-max`
- Included files:
  - `cpu_throttle.c` – source code
  - `cpu_throttle_ctl.c` – runtime control utility
  - `cpu_throttle` – compiled binary (available in [Releases](https://github.com/DiabloPower/burn2cool/releases))
  - `cpu_throttle_ctl` – control binary (available in [Releases](https://github.com/DiabloPower/burn2cool/releases))
  - `install_cpu-throttle.sh` – portable compiler/installer script with service integration (needs the source file)

> ⚡️ Don't need a service? Download the precompiled `cpu_throttle` binary from the [Releases page](https://github.com/DiabloPower/burn2cool/releases) — no compiling, no installation required. Just make it executable and launch it from the terminal.
>
> 💡 **Note:** Precompiled binaries are only available in the Releases section, not in the repository code itself.

### 2. `install_dynamic-tlp.sh` (Bash alternative - Legacy)
- **⚠️ Limited Feature Set:** Bash-based CPU throttle daemon - lacks runtime control, profiles, and config files
- **Recently modernized** with correct formulas, hysteresis, and proportional thresholds (matches C version's logic)
- Sets up a systemd service called `dynamic-tlp.service`
- **Use case:** Simple standalone solution or systems where compiling C code is problematic
- **Recommendation:** Use `cpu_throttle` for production - it offers runtime control, profile management, and better performance

**What was updated in the Bash version:**
- ✅ Fixed frequency calculation formulas (was incorrect before)
- ✅ Added 10% hysteresis to prevent frequency oscillation
- ✅ Proportional temperature thresholds (79%, 86%, 93% of TEMP_MAX)
- ✅ Configurable TEMP_MAX, SAFE_MIN, SAFE_MAX variables
- ✅ Better output formatting (MHz instead of kHz)
- ❌ Still lacks: runtime control, profiles, config files, logging levels

> ⚠️ You only need one: either the C-based `cpu_throttle` service or the Bash-based `dynamic-tlp` script. Both serve the same purpose — choose the one that fits your setup and preferences.

---

## 🚀 Installation

### Quick Start (Standalone Binary)

To use the C-based standalone binary, download it from the [Releases page](https://github.com/DiabloPower/burn2cool/releases):

```bash
# Download latest release binary
wget https://github.com/DiabloPower/burn2cool/releases/download/release/cpu_throttle
chmod +x cpu_throttle
sudo ./cpu_throttle
```

> 💡 Precompiled binaries are **only available in Releases**, not in the repository source code.

For available command line options:
```bash
./cpu_throttle --help
```

### Service Installation

To install the C-based daemon as a service:

```bash
chmod +x install_cpu-throttle.sh
./install_cpu-throttle.sh
```

**What the installer does:**
- ✅ Auto-detects your Linux distribution (Debian, Fedora, Arch, openSUSE, Alpine)
- ✅ Installs required build dependencies automatically
- ✅ Compiles both `cpu_throttle` and `cpu_throttle_ctl`
- ✅ Installs binaries to `/usr/local/bin/`
- ✅ Creates systemd service configuration
- ✅ Handles updates: stops existing service, backs up old config, restarts cleanly
- ✅ Verifies successful service startup

To install the Bash-based daemon:

```bash
chmod +x install_dynamic-tlp.sh
./install_dynamic-tlp.sh
```

> 💡 Tip: If you just want to run the tool manually, feel free to use the `cpu_throttle` binary on its own — no service setup required.

---

## ⚙️ Configuration

### Global Config File

Create `/etc/cpu_throttle.conf` (requires root) for system-wide defaults:

```ini
# Temperature threshold (50-110°C)
temp_max=85

# Frequency limits in kHz
safe_min=2000000
safe_max=3500000

# Optional: custom temperature sensor
sensor=/sys/class/thermal/thermal_zone0/temp
```

> 💡 Command-line arguments override config file settings.

---

## 🎮 Runtime Control (NEW!)

Change CPU throttling behavior on-the-fly without restarting the daemon!

> ⚠️ **Note:** Socket permissions are set to 0666, so you generally don't need `sudo` for control commands. If you still get connection errors, try with `sudo`.

### Basic Usage

```bash
# Set maximum frequency (e.g., for gaming)
cpu_throttle_ctl set-safe-max 4000000

# Set minimum frequency
cpu_throttle_ctl set-safe-min 2000000

# Change maximum temperature threshold
cpu_throttle_ctl set-temp-max 85

# Query current status
cpu_throttle_ctl status

# Shutdown daemon gracefully
cpu_throttle_ctl quit
```

### Profile Management

Save and load frequency profiles for different use cases:

```bash
# Save current settings as a profile
cpu_throttle_ctl save-profile gaming

# Load a profile
cpu_throttle_ctl load-profile gaming

# List all profiles
cpu_throttle_ctl list-profiles

# Delete a profile
cpu_throttle_ctl delete-profile oldprofile
```

> 💡 Profiles are stored in `~/.config/cpu_throttle/profiles/` (per-user)

### Example Workflow

```bash
# 1. Start daemon in background
sudo ./cpu_throttle &

# 2. Create a gaming profile with high frequencies
cpu_throttle_ctl set-safe-max 4000000
cpu_throttle_ctl save-profile gaming

# 3. Create a power-saving profile
cpu_throttle_ctl set-safe-max 2000000
cpu_throttle_ctl save-profile powersave

# 4. Switch profiles on-the-fly
cpu_throttle_ctl load-profile gaming      # Gaming time!
cpu_throttle_ctl load-profile powersave  # Back to power saving

# 5. Check current status
cpu_throttle_ctl status
```

---

## 📊 Logging Levels

Control daemon output verbosity:

```bash
# Verbose output (show all decisions)
sudo ./cpu_throttle --verbose

# Quiet mode (errors only)
sudo ./cpu_throttle --quiet

# Silent mode (no output)
sudo ./cpu_throttle --silent

# Default is normal output (key events + errors)
sudo ./cpu_throttle
```

**Use cases:**
- `--verbose` — Debugging, understanding throttling behavior
- `--quiet` — Production daemons (systemd journals)
- `--silent` — Minimal logging when performance matters

---

## 🔧 Command-Line Options

```
Usage: cpu_throttle [OPTIONS]

Temperature Control:
  --temp-max <temp>     Maximum temperature in °C (default: 95)
                        Thresholds scale proportionally with this value

Frequency Limits:
  --safe-max <freq>     Maximum CPU frequency in kHz
  --safe-min <freq>     Minimum CPU frequency in kHz

Logging:
  --verbose            Show all throttling decisions
  --quiet              Errors only
  --silent             No output

Testing:
  --dry-run            Show actions without applying changes
  --help               Show this help message
```

### Available Commands

**Frequency Control:**
- `set-safe-max <freq>` — Set maximum frequency in kHz
- `set-safe-min <freq>` — Set minimum frequency in kHz  
- `set-temp-max <temp>` — Set maximum temperature in °C (50-110)

**Status:**
- `status` — Show current temperature, frequency, and settings

**Profiles:**
- `save-profile <name>` — Save current settings to a profile
- `load-profile <name>` — Load settings from a profile
- `list-profiles` — Show all saved profiles
- `delete-profile <name>` — Delete a profile

**Daemon:**
- `quit` — Gracefully shutdown the daemon

---

## 🛠️ Technical Details

### Architecture
- **Daemon**: Temperature monitoring and frequency scaling loop
- **Control Client**: Unix socket communication for runtime control
- **IPC**: Unix Domain Socket at `/tmp/cpu_throttle.sock` (mode 0666)
- **PID File**: `/var/run/cpu_throttle.pid` for service management

### File System Integration
- **CPUFreq Interface**: `/sys/devices/system/cpu/cpu*/cpufreq/`
- **Thermal Sensors**: `/sys/class/thermal/thermal_zone*/temp`
- **Global Config**: `/etc/cpu_throttle.conf` (system-wide defaults)
- **User Profiles**: `~/.config/cpu_throttle/profiles/*.conf`

### Frequency Calculation
```
target_freq = min_freq + (max_freq - min_freq) × percentage / 100
```

Hysteresis prevents rapid changes (10% of frequency range required).

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
