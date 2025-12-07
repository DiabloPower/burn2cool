# Burn2Cool Tray (GTK)

Minimal tray application for interacting with the cpu_throttle daemon via its REST API.

Features (MVP):
- Tray icon with menu
- Profiles submenu (fetched from daemon)
- Click a profile to load it (via `/api/command`)
- Status entry showing temperature and frequency (updated periodically)
- Refresh, Open Web UI, Language selector, About, Quit

Build
```
cd gui_tray
make
./burn2cool_tray
```


Run
./burn2cool_tray
```
# CPU Throttle Tray (GTK)

Minimal tray application for interacting with the cpu_throttle daemon via its REST API.

Features (MVP):
- Tray icon with menu
- Profiles submenu (fetched from daemon)
- Click a profile to load it (via `/api/command`)
- Status entry showing temperature and frequency (updated periodically)
- Refresh, Open Web UI, Language selector, About, Quit

Build
```
cd gui_tray
make
```

# Run against a daemon on port 9090 and force German UI
./burn2cool_tray --port 9090 --lang de

```
./burn2cool_tray --config /tmp/my-tray-config.json

Dependencies
./burn2cool_tray --help

- libayatana-appindicator3 (or compatible AppIndicator)
./burn2cool_tray --version
- json-c
- libnotify

Notes & Troubleshooting
- If you see a GTK message like: `Failed to load module "xapp-gtk3-module"` this is a harmless warning coming from some desktop environments. Install the XApp GTK3 module for your distro to remove the warning (package name varies).
 - If `xdg-open` reports an "unexpected argument '&'" error, update to a current `burn2cool_tray` binary — the launcher no longer appends `&` when launching the browser.

Notes
- The tray talks to the daemon on `http://localhost:8086` by default. If your daemon uses another port, pass `--port <port>` to the binary.

### Flags

- `--port <port>`: set the daemon HTTP port (default: `8086`). Example: `./burn2cool_tray --port 9090`
- `--lang <code>`: force the UI language by locale code (overrides environment and saved user preference). Example: `./burn2cool_tray --lang de`
- `--config <path>`: use a custom per-user config file (the app reads/writes `{"lang": "<code>"}` to persist the language). If omitted the app uses `~/.config/cpu_throttle_gui/.config`.
- `-h, --help`: show usage and exit.
- `-V, --version`: print program version and exit.

Precedence for language selection (highest → lowest):

1. `--lang <code>` CLI flag (if provided)
2. `--config <path>` file contents (if provided) or `~/.config/cpu_throttle_gui/.config` (if present)
3. `CPU_THROTTLE_LANG` environment variable
4. `LANG` environment variable (e.g. `en_US.UTF-8` → `en`)
5. fallback to `en`

Examples

```bash
# Run against a daemon on port 9090 and force German UI
./burn2cool_tray --port 9090 --lang de

# Use a custom config file (useful for testing persistence)
./burn2cool_tray --config /tmp/my-tray-config.json

# Show help/usage
./burn2cool_tray --help

# Print version
./burn2cool_tray --version
```

### Internationalization (i18n)

This tray app supports JSON-based localization files stored in `gui_tray/i18n/`.

- Default file: `gui_tray/i18n/en.json`.
- File pattern: `i18n/<lang>.json` (e.g. `i18n/de.json`).
- The app reads the desired language from the environment variable `CPU_THROTTLE_LANG` (if set) or falls back to `LANG` (e.g. `en_US.UTF-8` → `en`). If a language file is missing the app falls back to English (`en`).

CLI override and persistence
- You can force a language on startup with `--lang <code>` (e.g. `--lang de`). This overrides environment and persisted user preference.
- When a user selects a language in the tray menu the choice is persisted to a per-user config file at `~/.config/cpu_throttle_gui/.config` (JSON: `{ "lang": "de" }`). On subsequent starts the app will use that saved language unless overridden by `--lang`.
- You may also pass `--config <path>` to use a custom config file location (useful for testing). If provided, the path is used directly instead of `~/.config/cpu_throttle_gui/.config`.

Validation script
- A small script `scripts/validate-i18n.sh` checks all JSON files in `gui_tray/i18n` for valid JSON and that they contain the same keys as `en.json`. Run it from repository root:

```bash
./gui_tray/scripts/validate-i18n.sh
```

About / Version
- The tray menu contains a non-selectable version label shown under the status line (e.g. `Version: v0.3.0`).
- The tray also offers an "About" menu item which opens a GTK About dialog showing:
  - Program name and version
  - Authors
  - Repository / website link (GitHub)

Note: Version strings are managed manually for releases; update `APP_VERSION` in `gui_tray/burn2cool_tray.c` when creating a new release.

Custom About image and tray icon
- If you have a larger artwork you'd like to show in the About dialog, place it as `gui_tray/assets/about.png` (PNG recommended). The tray will automatically load that file into the About dialog when present.
- The tray will also attempt to use the project's `assets/favicon.ico` (if present and compiled into `include/favicon_ico.h`) as the AppIndicator/tray icon. No additional configuration is required for that — the embedded favicon will be written to the per-user config directory and used at runtime.

Distribution note — installing icon on target systems
If you distribute only the compiled binary, include a small PNG (`icon.png`) alongside it and provide users with a simple installer script to register the icon with their user icon theme. This repo includes `scripts/install-user-icon.sh` which copies a provided PNG to `~/.local/share/icons/hicolor/32x32/apps/cpu-throttle.png` and attempts to update the icon cache.

Example for packagers / releases:
1. Ship the binary `burn2cool_tray` and `icon.png` in a tarball.
2. Instruct users to run:

```bash
./burn2cool_tray --port 8086 &
./gui_tray/scripts/install-user-icon.sh ./icon.png
```

This ensures the icon is available to the desktop shell even when the application was not compiled on the target host.

Structure (top-level keys):
- `menu` — menu labels and small UI texts
- `status` — status line strings and format (e.g. `"format": "Status: %d°C, %d kHz"`)
- `messages` — notification titles and bodies

Quick example (see `i18n/en.json` in the repo):

```json
{
  "menu": { "overview": "Overview", "quit": "Quit" },
  "status": { "offline": "Status: Offline", "format": "Status: %d°C, %d kHz" },
  "messages": { "loaded_profile_title": "Loaded profile", "loaded_profile_body": "Profile '%s' is now active." }
}
```

Add a new language
1. Copy `i18n/en.json` to `i18n/<code>.json` and translate the strings.
2. Test with:

```bash
# CPU Throttle Tray (GTK)

Minimal tray application for interacting with the cpu_throttle daemon via its REST API.

Features (MVP):
- Tray icon with menu
- Profiles submenu (fetched from daemon)
- Click a profile to load it (via `/api/command`)
- Status entry showing temperature and frequency (updated periodically)
 - Overview, Open Web UI, Language selector, About, Quit

Build
```
cd gui_tray
make
```

Run
```
./burn2cool_tray
```

Dependencies
- GTK+3 development files
- libayatana-appindicator3 (or compatible AppIndicator)
- libcurl
- json-c
- libnotify

Notes & Troubleshooting
- If you see a GTK message like: `Failed to load module "xapp-gtk3-module"` this is a harmless warning coming from some desktop environments. Install the XApp GTK3 module for your distro to remove the warning (package name varies).
 - If `xdg-open` reports an "unexpected argument '&'" error, update to a current `burn2cool_tray` binary — the launcher no longer appends `&` when launching the browser.

Notes
- The tray talks to the daemon on `http://localhost:8086` by default. If your daemon uses another port, pass `--port <port>` to the binary.

### Flags

- `--port <port>`: set the daemon HTTP port (default: `8086`). Example: `./burn2cool_tray --port 9090`
- `--lang <code>`: force the UI language by locale code (overrides environment and saved user preference). Example: `./burn2cool_tray --lang de`
- `--config <path>`: use a custom per-user config file (the app reads/writes `{"lang": "<code>"}` to persist the language). If omitted the app uses `~/.config/cpu_throttle_gui/.config`.
- `-h, --help`: show usage and exit.
- `-V, --version`: print program version and exit.

Precedence for language selection (highest → lowest):

1. `--lang <code>` CLI flag (if provided)
2. `--config <path>` file contents (if provided) or `~/.config/cpu_throttle_gui/.config` (if present)
3. `CPU_THROTTLE_LANG` environment variable
4. `LANG` environment variable (e.g. `en_US.UTF-8` → `en`)
5. fallback to `en`

Examples

```bash
# Run against a daemon on port 9090 and force German UI
./burn2cool_tray --port 9090 --lang de

# Use a custom config file (useful for testing persistence)
./burn2cool_tray --config /tmp/my-tray-config.json

# Show help/usage
./burn2cool_tray --help

# Print version
./burn2cool_tray --version
```

### Internationalization (i18n)

This tray app supports JSON-based localization files stored in `gui_tray/i18n/`.

- Default file: `gui_tray/i18n/en.json`.
- File pattern: `i18n/<lang>.json` (e.g. `i18n/de.json`).
- The app reads the desired language from the environment variable `CPU_THROTTLE_LANG` (if set) or falls back to `LANG` (e.g. `en_US.UTF-8` → `en`). If a language file is missing the app falls back to English (`en`).

CLI override and persistence
- You can force a language on startup with `--lang <code>` (e.g. `--lang de`). This overrides environment and persisted user preference.
- When a user selects a language in the tray menu the choice is persisted to a per-user config file at `~/.config/cpu_throttle_gui/.config` (JSON: `{ "lang": "de" }`). On subsequent starts the app will use that saved language unless overridden by `--lang`.
- You may also pass `--config <path>` to use a custom config file location (useful for testing). If provided, the path is used directly instead of `~/.config/cpu_throttle_gui/.config`.

Validation script
- A small script `scripts/validate-i18n.sh` checks all JSON files in `gui_tray/i18n` for valid JSON and that they contain the same keys as `en.json`. Run it from repository root:

```bash
./gui_tray/scripts/validate-i18n.sh
```

About / Version
- The tray menu shows the program version as a non-selectable label under the status line (e.g. `Version: v0.3.0`).
- The tray also offers an "About" menu item which shows a short desktop notification with the program version and current daemon port.

Structure (top-level keys):
- `menu` — menu labels and small UI texts
- `status` — status line strings and format (e.g. `"format": "Status: %d°C, %d kHz"`)
- `messages` — notification titles and bodies

Quick example (see `i18n/en.json` in the repo):

```json
{
  "menu": { "refresh": "Refresh", "quit": "Quit" },
  "status": { "offline": "Status: Offline", "format": "Status: %d°C, %d kHz" },
  "messages": { "loaded_profile_title": "Loaded profile", "loaded_profile_body": "Profile '%s' is now active." }
}
```

Add a new language
1. Copy `i18n/en.json` to `i18n/<code>.json` and translate the strings.
2. Test with:

```bash
CPU_THROTTLE_LANG=de ./gui_tray/burn2cool_tray --port 8086 &
```

Notes
- If a localization key is missing at runtime, the code uses a sensible English fallback for that string.
