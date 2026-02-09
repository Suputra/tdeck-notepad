# T-Deck Pro Notepad

Firmware for the LilyGo T-Deck Pro (ESP32-S3, 3.1" e-ink, keyboard) — a pocket notepad with SSH terminal, file manager, WireGuard VPN, and 4G modem support.

## Build & Flash

Requires [PlatformIO](https://platformio.org/). All dependencies are fetched automatically on first build.

```bash
pio run              # build
pio run -t upload    # flash via USB-C
pio device monitor   # serial output
```

## SD Card Config

Credentials are loaded from a `/CONFIG` file on the SD card (FAT32). One value per line, `#` comments, blank lines ignored.

```
# wifi
my_ssid
my_password
# ssh
10.0.0.100
22
user
password
# vpn (omit or set DISABLE to skip)
ENABLE
<device_private_key_base64>
<server_public_key_base64>
<device_vpn_ip>
<endpoint_host_or_ip>
51820
```

## Usage

### Notepad (default mode)

Type on the keyboard. Text wraps to the e-ink display.

- **Shift** — sticky uppercase (one letter)
- **Sym** — one-shot symbol/number layer
- **Alt** — toggles nav mode (WASD = arrows, backspace = delete forward)
- **Alt+F** — force full e-ink refresh

### Terminal

Double-tap **MIC** to switch to terminal mode. The device connects WiFi, then VPN (if enabled), then SSH — all automatically. Double-tap **MIC** again to return to notepad.

- **Alt+Q** — disconnect SSH
- **Alt+W** — reconnect WiFi
- **Alt+P** — paste notepad buffer into SSH session
- **Alt+F** — force full e-ink refresh
- **Alt+Space** — send Escape
- **Alt+Enter** — send Tab
- **Alt+letter** — send Ctrl+letter

### Command Processor

Single-tap **MIC** from either mode to open the command prompt (bottom half of screen).

| Command | Description |
|---------|-------------|
| `l` / `ls` | List files on SD card |
| `e` / `edit <file>` | Edit a file (loads into notepad) |
| `w` / `save` | Save notepad to current file |
| `n` / `new` | New file (auto-saves current) |
| `r` / `rm <file>` | Delete a file |
| `u` / `upload` | SCP all SD files to `~/tdeck` on SSH host |
| `d` / `download` | SCP `~/tdeck` files to SD card |
| `p` / `paste` | Paste notepad to SSH |
| `dc` | Disconnect SSH |
| `f` / `refresh` | Force full e-ink refresh |
| `4` / `4g` | Toggle 4G modem |
| `s` / `status` | Show WiFi/SSH/VPN/battery status |
| `?` / `h` / `help` | Show help |

### 4G Modem

**Alt+M** toggles the A7682E cellular modem (off by default). Currently supports AT commands and network registration. PPPoS for SSH-over-cellular is planned.

## Architecture

Single-file firmware (`src/main.cpp`). Two FreeRTOS cores:

- **Core 0** — e-ink display rendering
- **Core 1** — keyboard polling, WiFi/SSH/VPN, file I/O

SPI bus shared between e-ink and SD card via cooperative `sd_busy`/`display_idle` flags (no mutex).
