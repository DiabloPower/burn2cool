# Burn2Cool — The ROG Tamer CPU Throttle Daemon

A lightweight Linux service that dynamically throttles CPU frequency based on temperature. Originally designed to tame the powerful (and sometimes thermally confused) **ASUS ROG Strix Hero III**, it works on most Linux systems with standard thermal zones and CPUFreq drivers.

---

## 🔧 What's Inside

### 1. `cpu_throttle` (C program)
- Dynamically adjusts CPU max frequency between 70°C and 95°C
- Uses min/max freq from sysfs (`cpuinfo_min_freq`, `cpuinfo_max_freq`)
- Fast and efficient: suitable for long-term background use
- Optional flags: `--dry-run`, `--log`, `--sensor`
- Included files:
  - `cpu_throttle.c` – source code
  - `cpu_throttle` – compiled binary
  - `install_cpu-throttle.sh` – portable compiler/installer script with service integration (needs the source file)

### 2. `install_dynamic-tlp.sh` (Bash alternative)
- Bash-based CPU throttle daemon, also governed by temperature
- Independent of the C program and easy to modify
- Sets up a systemd service called `dynamic-tlp.service`
- Great for experimentation or fallback use

---

## 🚀 Installation

To install the C-based daemon:

```bash
chmod +x install_cpu-throttle.sh
./install_cpu-throttle.sh
```

To install the Bash-based daemon:

```bash
chmod +x install_dynamic-tlp.sh
./install_dynamic-tlp.sh
```
