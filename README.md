# T-Deck Pro Notepad

Firmware for the LilyGo T-Deck Pro — a pocket notepad with SSH terminal, file manager, WireGuard VPN, and BLE HID keyboard phone pairing.

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
TDeck-Pro
# optional 6-digit static passkey for secure pairing
123456
```

Open WiFi entries: put only the SSID and leave the password line blank (or end the WiFi section right after the SSID).

If `# bt` is enabled, the device advertises as a BLE HID keyboard and keeps advertising whenever disconnected. After first bonding/pairing with your phone, iOS can reconnect using standard keyboard behavior.
`# bt` parsing is positional:
1. Device name
2. Optional 6-digit passkey

If `# bt` section is missing, BLE stays disabled.
HID access requires encryption, so iOS should prompt to pair/bond on first access.

## Usage

### Notepad (default mode)

Type on the keyboard. Text wraps to the e-ink display.

- **Shift** — sticky uppercase (one letter)
- **Sym** — one-shot symbol/number layer
- **Alt** — toggles nav mode (WASD = arrows, backspace = delete forward)

### Terminal

Double-tap **MIC** to switch to terminal mode. The device connects WiFi, tries SSH directly, and if the host isn't reachable falls back through WireGuard VPN automatically. Double-tap **MIC** again to return to notepad.

- **Alt** — acts as ctrl - alt + space -> esc

### Keyboard Mode (BLE HID)

Single-tap **MIC** to open command mode, run `k` to enter keyboard mode, then keypresses are sent to the paired phone over BLE HID.

- **MIC** single tap: open command prompt
- `k` again from command prompt: exit keyboard mode
- **Alt** acts as Command modifier for phone shortcuts
- `p` in command mode while returning to keyboard mode pastes the current notepad buffer to the phone

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
| `k` / `keyboard` | Toggle BLE keyboard mode on/off |
| `p` / `paste` | Paste notepad to SSH, or to BLE phone when returning to keyboard mode |
| `dc` | Disconnect SSH |
| `ws` / `scan` | Scan WiFi and retry known APs manually |
| `bt` / `bluetooth` | BLE status (`bt status`), send typed text (`bt send <txt>`) |
| `f` / `refresh` | Force full e-ink refresh |
| `s` / `status` | Show WiFi/SSH/VPN/battery status |
| `?` / `h` / `help` | Show help |

## Architecture

Firmware entry point remains `src/main.cpp` (setup/loop and global state), while major firmware components are split into flat module headers in `src/`:

- `src/network_module.hpp` (WiFi, SSH, VPN connectivity)
- `src/bluetooth_module.hpp` (BLE HID keyboard, pairing/bonding, auto-advertise, keepalive typing)
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
