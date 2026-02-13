# T-Deck Pro Notepad

Firmware for the LilyGo T-Deck Pro — a pocket notepad with SSH terminal, file manager, WireGuard VPN, and optional barebones BLE peripheral pairing.

## Build & Flash

Requires [PlatformIO](https://platformio.org/). All dependencies are fetched automatically on first build.

### Quick setup (`uv`, no virtualenv)

Run this once:

```bash
uv tool install --force platformio && uv tool update-shell && export PATH="$(uv tool dir --bin):$PATH" && hash -r
```

After that, use plain `pio` commands:

```bash
pio run              # build
pio run -t upload    # flash via USB-C
pio device monitor   # serial output
```

### Debug automation build

Two PlatformIO environments are available:

```bash
pio run -e T-Deck-Pro -t upload        # production (agent protocol disabled)
pio run -e T-Deck-Pro-debug -t upload  # debug (agent protocol enabled)
```

With debug firmware flashed, you can drive the device over serial:

```bash
uv sync
uv run scripts/tdeck_agent.py --boot-wait 2 "PING" "STATE"
uv run scripts/tdeck_agent.py "MIC SINGLE" "WAIT 500" "STATE"
uv run scripts/tdeck_agent.py "MIC DOUBLE" "WAIT 300" "STATE"
```

You can also capture screen evidence from the default IP camera feed (`http://10.0.44.199:4747/`):

```bash
uv run scripts/capture_webcam.py --image artifacts/screen.jpg
```

Recommended end-to-end smoke command (write + render + capture + checks):

```bash
uv run scripts/agent_smoke.py --boot-wait 2
```

To use a different camera source:

```bash
uv run scripts/agent_smoke.py --camera-source "http://<ip>:4747/" --boot-wait 2
uv run scripts/capture_webcam.py --source "0" --image artifacts/usb-webcam.jpg
```

If a local webcam index is wrong or captures are black:

```bash
uv run scripts/probe_cameras.py --max-index 5
```

Alternative setup script:

```bash
source scripts/setup-pio.sh
```

## SD Card Config

Credentials are loaded from a `/CONFIG` file on the SD card (FAT32). Section-based format with `#` comments and blank lines ignored. Section headers are `# wifi`, `# ssh`, `# vpn`, `# bt`.

```
# wifi (SSID/password pairs; blank or missing password means open WiFi)
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
# optional: host to use only when SSH is attempted via VPN
# (useful if direct host is a local shortname that doesn't resolve over tunnel DNS)
10.207.162.10

# vpn (omit section if not using VPN)
<device_private_key_base64>
<server_public_key_base64>
<preshared_key_base64>
<device_vpn_ip>
<endpoint_host_or_ip>
51820
# optional: DNS server to use while VPN is active
10.0.0.1

# bt (optional)
# optional auto-enable at boot; otherwise BT stays off until `bt on`
enable
TDeck-Pro
# optional 6-digit static passkey for secure pairing
123456
```

Open WiFi entries: put only the SSID and leave the password line blank (or end the WiFi section right after the SSID).

`# bt` parsing is positional:
1. Optional `enable`/`on`/`true`/`1` to auto-enable Bluetooth at boot
2. Device name
3. Optional 6-digit passkey

If `# bt` section is missing, BLE stays disabled.
If BT is enabled, the device advertises as a generic BLE peripheral (not a keyboard), so phones keep their on-screen keyboard.

## Usage

### Notepad (default mode)

Type on the keyboard. Text wraps to the e-ink display.

- **Shift** — sticky uppercase (one letter)
- **Sym** — one-shot symbol/number layer
- **Alt** — toggles nav mode (WASD = arrows, backspace = delete forward)

### Terminal

Double-tap **MIC** to switch to terminal mode. The device connects WiFi, tries SSH directly, and if the host isn't reachable falls back through WireGuard VPN automatically. Double-tap **MIC** again to return to notepad.

- **Alt** — acts as ctrl - alt + space -> esc

### Bluetooth (bare mode)

Bluetooth is runtime-toggleable from the command prompt with a single command:

- `bt` toggles Bluetooth on/off

To reduce unwanted phone wakes, advertising is not kept alive indefinitely.

### Command Processor

Single-tap **MIC** from either mode to open the command prompt (bottom half of screen).

| Command | Description |
|---------|-------------|
| `l` / `ls` | List files on SD card |
| `e` / `edit [file]` | Edit a file (loads into notepad). With no filename, opens interactive picker (W/S move, A/D page, Enter open). |
| `w` / `save` | Save notepad to current file |
| `n` / `new` | New file (auto-saves current) |
| `r` / `rm <file>` | Delete a file |
| `u` / `upload` | SCP all SD files to `~/tdeck` on SSH host |
| `d` / `download` | SCP `~/tdeck` files to SD card |
| `p` / `paste` | Paste notepad to SSH |
| `dc` | Disconnect SSH |
| `ws` / `scan` | Scan WiFi and retry known APs manually |
| `bt` / `bluetooth` | Toggle Bluetooth on/off |
| `f` / `refresh` | Force full e-ink refresh |
| `s` / `status` | Show WiFi/SSH/VPN/battery status (includes BT name/pair/peer) |
| `?` / `h` / `help` | Show help |

## Architecture

Firmware entry point remains `src/main.cpp` (setup/loop and global state), while major firmware components are split into flat module headers in `src/`:

- `src/network_module.hpp` (WiFi, SSH, VPN connectivity)
- `src/bluetooth_module.hpp` (barebones BLE peripheral, pairing/bonding, runtime toggle)
- `src/screen_module.hpp` (display rendering/task logic)
- `src/keyboard_module.hpp` (keyboard input + mode handlers)
- `src/cli_module.hpp` (command parsing, SCP helpers, poweroff flow)

Shared configuration/constants are split into:

- `src/firmware/pins.h`
- `src/firmware/layout.h`
- `src/firmware/keyboard_map.h`
- `src/firmware/network_config.h`

Runtime uses two FreeRTOS cores:

- **Core 0** — e-ink display rendering
- **Core 1** — keyboard polling, WiFi/SSH/VPN/BLE, file I/O

SPI bus shared between e-ink and SD card via cooperative `sd_busy`/`display_idle` flags (no mutex).
