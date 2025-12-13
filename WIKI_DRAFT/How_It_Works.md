# How It Works

This page explains the internal workings of the burn2cool CPU throttle daemon, including where temperature data comes from, when it's read, why throttling occurs, how frequencies are calculated, and what actions are taken.

## Data Sources

Temperature data is sourced from the Linux Thermal Subsystem, specifically from files like `/sys/class/thermal/thermal_zone*/temp`. These files provide temperature readings in millidegrees Celsius (e.g., 45000 for 45°C).

- **Default behavior**: The daemon auto-detects the best CPU thermal zone (typically `thermal_zone0`) by scanning available zones and preferring those with "cpu" or "x86_pkg_temp" in their type.
- **Manual override**: Use `--thermal-zone <num>` to specify a particular zone.
- **Average mode**: With `--avg-temp`, the daemon averages temperatures across all CPU-related thermal zones for more accurate readings. Certain types can be excluded via `excluded_types` (e.g., "int3400,wifi").

## Timing

- **Temperature polling**: Every 1000 ms (1 second) in the main daemon loop.
- **Socket polling**: Every 250 ms for handling control commands via Unix socket or HTTP.
- **Frequency updates**: Only when the calculated frequency changes significantly (>10% of the frequency range) to avoid excessive writes.

## Why Throttling?

The daemon dynamically adjusts the CPU's maximum frequency to prevent overheating while maintaining performance. Throttling starts early (30°C below `temp_max`) for a gentle curve, reducing power consumption and heat generation as temperatures rise.

## Calculation Logic

The daemon uses linear scaling with hysteresis:

- **Throttling range**: From `temp_max - 30°C` (e.g., 65°C) to `temp_max` (e.g., 95°C).
- **Frequency scaling**: Linear from 100% of max_freq at the start to 50% of max_freq at `temp_max`.
- **Formula**:
  ```
  throttle_start = temp_max - 30
  if temp >= temp_max:
      target_freq = min_freq (or safe_min if set)
  elif temp >= throttle_start:
      temp_range = temp_max - throttle_start
      freq_range = max_freq / 2
      temp_above_start = temp - throttle_start
      target_freq = max_freq - (freq_range * temp_above_start) / temp_range
      if target_freq < safe_min: target_freq = safe_min
  else:
      target_freq = max_freq
  ```
- **Hysteresis**: Changes only occur if the temperature deviates by ≥3°C from the last throttle point, preventing oscillations.
- **Safe limits**: `safe_min` and `safe_max` clamp the frequency range.

## What Happens

- **Frequency adjustment**: Writes the target frequency to `/sys/devices/system/cpu/cpu*/cpufreq/scaling_max_freq` for each CPU core.
- **Logging**: Outputs to console or log file (e.g., "Temp: 75°C → MaxFreq: 2500000 kHz").
- **Runtime control**: Accepts commands via Unix socket (`/tmp/cpu_throttle.sock`) or HTTP API for live adjustments.
- **Emergency mode**: At ≥`temp_max`, immediately drops to minimum frequency.

This ensures smooth, efficient CPU management without abrupt performance drops.