# T-Deck Pro Notepad

Firmware for the LilyGo T-Deck Pro: e-ink notepad, SSH terminal, SD-card file workflow, optional WireGuard fallback, and optional BLE/GPS/4G features.

## Overview
- Default mode is a keyboard-driven notepad rendered on the e-ink panel.
- `ssh` switches to terminal mode (WiFi first, then VPN fallback when configured).
- Files live on SD root and can be edited/saved on-device or transferred with SCP mirror sync (`upload` / `download`).
- Bluetooth mode is a BLE peripheral service (not a HID keyboard), so phones keep their on-screen keyboard.

## Quickstart
### 1) Install tools
Requires PlatformIO (`pio`) and `uv`.

If `pio` is not already available:

```bash
uv tool install --force platformio
uv tool update-shell
export PATH="$(uv tool dir --bin):$PATH"
hash -r
```

Install Python deps used by helper scripts:

```bash
uv sync
```

Alternative setup helper:

```bash
source scripts/setup-pio.sh
```

### 2) Flash production firmware
```bash
pio run -t upload
```

(Optional serial output)

```bash
pio device monitor
```

### 3) Add SD card config
Create `/CONFIG` on a FAT32 SD card (format below), insert card, and reboot.

### 4) Start using the device
Type in notepad mode. Single-tap `MIC` to open command mode. Use `h` for command help.

For debug automation and camera capture flow, see `Development` at the bottom.

## SD Card Configuration (`/CONFIG`)
`/CONFIG` is section-based. Section lines start with `#` and include one of: `wifi`, `ssh`, `vpn`, `bt`, `time`.

Example:

```text
# wifi
home_ssid
home_password
office_guest_open

phone_hotspot
hotspot_password

# ssh
10.0.0.100
22
user
password
10.207.162.10

# vpn
<device_private_key_base64>
<server_public_key_base64>
<preshared_key_base64>
<device_vpn_ip>
<endpoint_host_or_ip>
51820
10.0.0.1

# bt
enable
TDeck-Pro
123456

# time
PST8PDT,M3.2.0,M11.1.0
```

Notes:
- `# wifi`: lines are SSID/password pairs. If password is blank (or section ends right after SSID), that AP is treated as open.
- `# ssh`: host, port, user, password, optional VPN-only host override.
- `# vpn`: private key, server pubkey, PSK, local VPN IP, endpoint, port, optional DNS.
- `# bt`: parsed in order as optional boot-state token (`enable`/`on`/`true`/`1` or `disable`/`off`/`false`/`0`), optional device name, optional 6-digit passkey.
- If `# bt` is missing, Bluetooth stays off at boot. Runtime control is the `bt` command (toggle only).
- If `# time` is missing, timezone defaults to `UTC0`.

## Usage
### Notepad (default mode)
Type on the keyboard. Text wraps to the e-ink display.

- `Shift`: sticky uppercase (one letter)
- `Sym`: one-shot symbol/number layer
- Touch tap: directional arrows (tap away from center for up/down/left/right)

### Terminal
Run `ssh` from command mode to switch to terminal mode. Device connects WiFi, tries SSH directly, and falls back through WireGuard when configured and needed. Run `np` to return to notepad.

- `Alt`: acts as Ctrl (`Alt + Space` sends Esc)
- Touch tap: sends terminal arrow keys

### Bluetooth (bare mode)
Bluetooth is runtime-toggleable from command mode:

- `bt`: toggle Bluetooth on/off
- `bs`: scan nearby BLE devices

Advertising is not kept alive indefinitely (times out to idle) to reduce unwanted wakeups.

### Command Processor
Single-tap `MIC` from any mode to open command mode (bottom half of screen).
Touch arrows in command mode browse command history. In file-picker mode: Up/Down moves, Left/Right pages.

| Command | Description |
|---------|-------------|
| `l` / `ls` | List files on SD card |
| `e` / `edit [file]` | Edit a file. With no filename, opens interactive picker (W/S move, A/D page, Enter open). |
| `w` / `save [file]` | Save notepad to current file (or provided filename) |
| `daily` | Open today’s file as `YYYY-MM-DD.md` (local timezone) |
| `r` / `rm <file>` | Delete a file |
| `u` / `upload` | Mirror SD root to `~/tdeck` on SSH host (overwrite + delete extras on host) |
| `d` / `download` | Mirror `~/tdeck` to SD root (overwrite + delete extras on SD) |
| `p` / `paste` | Paste notepad to SSH |
| `ssh` | Switch to terminal mode and connect if needed |
| `np` | Return to notepad mode |
| `dc` | Disconnect SSH |
| `ws` | Scan WiFi and retry known APs |
| `wfi` | Toggle WiFi on/off |
| `mds` | 4G modem status scan (non-blocking) |
| `mdm` | Toggle 4G modem power (non-blocking) |
| `bs` | Scan nearby BLE devices (non-blocking) |
| `bt` | Toggle Bluetooth on/off |
| `gps` | Toggle GPS on/off |
| `gs` / `gpss` | GPS detail scan (non-blocking) |
| `date` | Show local date/time and sync source |
| `s` / `status` | Show WiFi/4G/SSH/BT/GPS/battery/clock status |
| `h` / `help` | Show help |
| `<name>` or `<name>.x` | Run shortcut script from `/<name>.x` |

GPS is off by default. When GPS has valid UTC + fix, firmware auto-syncs system clock. NTP sync (over network/VPN) updates the same clock.

### `.x` Shortcut Scripts
Shortcut scripts are plain text files on SD root with extension `.x`.
Edit with `edit <name>.x`. Run with `<name>` (or `<name>.x`) in command mode.

Name resolution:
- `deploy` runs `/deploy.x`
- `deploy.x` runs `/deploy.x`
- Valid chars: `a-z`, `A-Z`, `0-9`, `.`, `_`, `-`
- Only one shortcut runs at a time

Parsing/execution:
- Blank lines and lines starting with `#` are ignored
- Max `24` executable lines per file
- Max line length `159` chars
- Steps execute in order and stop on first failure
- Progress appears in command result area (`Run <name> <i>/<n>`, then `Shortcut done: <name>`)

Supported steps:
- `upload` / `u`
- `download` / `d`
- `wait upload` / `wait download`
- `wait <ms>`
- `remote <command>` / `exec <command>`
- `cmd <command>`
- Any bare command-mode command (`daily`, `status`, etc.)

Remote-step notes:
- If SSH is disconnected, shortcut runtime will try to connect first
- SSH connect wait window is up to ~45s per remote step
- Non-zero remote exit code fails the shortcut (`Remote failed (<code>)`)
- If no status arrives before timeout, step fails as `Remote timeout`

Example `deploy.x`:

```text
upload
wait upload
remote mkdir -p "$HOME/app/config"
remote cp -f "$HOME/tdeck/"*.json "$HOME/app/config/"
```

Example `daily-sync.x`:

```text
daily
save
upload
wait upload
np
```

## Architecture
Entry point is `src/main.cpp` (setup/loop and global state).
Major firmware components are split into flat module headers in `src/`:

- `src/network_module.hpp` (WiFi, SSH, VPN connectivity)
- `src/modem_module.hpp` (A7682E modem power + LTE scan helpers)
- `src/bluetooth_module.hpp` (BLE peripheral, pairing/bonding, runtime toggle)
- `src/screen_module.hpp` (display rendering/task logic)
- `src/keyboard_module.hpp` (keyboard input + mode handlers)
- `src/cli_module.hpp` (command parsing, SCP helpers, poweroff flow)

Shared config/constants:
- `src/firmware/pins.h`
- `src/firmware/layout.h`
- `src/firmware/keyboard_map.h`
- `src/firmware/network_config.h`

Runtime uses two FreeRTOS cores:
- Core 0: e-ink display rendering
- Core 1: keyboard polling, WiFi/SSH/VPN/BLE, file I/O

SPI bus is shared between e-ink and SD via cooperative `sd_busy` / `display_idle` flags.

## Development
### Build modes
- Production: `pio run -t upload`
- Debug automation: `pio run -e debug -t upload`

`debug` maps to `T-Deck-Pro-debug` and enables `TDECK_AGENT_DEBUG=1` (serial automation protocol).
Production keeps it disabled.

### Fast path (write + render + capture)
```bash
pio run -e debug -t upload
uv run scripts/agent_smoke.py --boot-wait 2
```

`agent_smoke.py`:
1. Clears notepad
2. Types a marker
3. Forces render + waits
4. Captures camera frame (default `http://10.0.44.199:4747/`)
5. Verifies serial `text_len`
6. Verifies capture is not black/invalid
7. Prints `PASS` / `FAIL`

Notes:
- No OCR yet; still visually confirm marker in the image.
- Output artifact path is printed.
- Custom marker should avoid `0` (current `TEXT` emulation limitation).

### Camera setup
If a local webcam source is wrong or black:

```bash
uv run scripts/probe_cameras.py --max-index 5
```

Pick an index with `opened=1`, `frame=1`, and non-trivial `mean/std`, then pass `--camera-source "<idx>"`.

### Full canonical loop
1. Flash debug firmware: `pio run -e debug -t upload`
2. Check serial channel: `uv run scripts/tdeck_agent.py --boot-wait 2 "PING" "STATE"`
3. Drive scenario commands over serial
4. Capture artifacts:
   - image (default feed): `uv run scripts/capture_webcam.py --image artifacts/<name>.jpg`
   - image (custom source): `uv run scripts/capture_webcam.py --source "<url-or-idx>" --image artifacts/<name>.jpg`
   - video (custom source): `uv run scripts/capture_webcam.py --source "<url-or-idx>" --video artifacts/<name>.mp4 --duration 8`
5. Evaluate:
   - no `AGENT ERR`
   - expected `AGENT OK STATE ...` transitions
   - expected screen content in image/video
6. Patch and repeat

### Serial protocol (debug firmware only)
Send one command per line with `@` prefix.

- `@PING`
- `@HELP`
- `@STATE`
- `@KEY <row> <col_rev>`
- `@PRESS <token> [count]`
- `@TEXT <text-with-escapes>`
- `@CMD <device-command-mode-command>`
- `@WAIT <ms>`
- `@RENDER`
- `@BOOTOFF`

Escapes in `TEXT`: `\\n`, `\\r`, `\\t`, `\\\\`, `\\s`.

`PRESS` tokens:
- Special: `MIC`, `ALT`, `SYM`, `LSHIFT`, `RSHIFT`, `SPACE`, `ENTER`, `BACKSPACE`
- Single-char physical key tokens: letters and symbol-key positions (`q`, `w`, `1`, `?`, `-`)

Examples:

```bash
uv run scripts/tdeck_agent.py "PRESS MIC" "WAIT 500" "STATE"
uv run scripts/tdeck_agent.py "CMD ssh" "WAIT 300" "STATE"
uv run scripts/tdeck_agent.py "CMD np" "WAIT 300" "STATE"
```

### Troubleshooting
- `AGENT ERR`: scenario failure, fix command or firmware behavior.
- Camera opens but frame is black: verify stream URL or probe camera indices and increase `--warmup`.
- `TEXT` fails due to active modifiers: use explicit `KEY`/`PRESS` first.
- Need release validation: flash production env and rerun manual/non-agent checks.
