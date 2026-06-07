# reTerminal E1001 companion build

The same firmware runs on the Seeed **reTerminal E1001** (7.5″ 800×480 mono e-paper,
ESP32-S3, no keyboard, no touch) as a laptop companion: an e-ink notepad / SSH
terminal whose keystrokes arrive **from your laptop over BLE**. A hotkey on the
laptop flips all typing to the device; another opens its command palette.

It is the same codebase as the T-Deck Pro, selected at build time. T-Deck radios
(GPS / 4G / Meshtastic / LoRa) and the physical keyboard/touch are compiled out;
WiFi, SSH, WireGuard, the SD `/CONFIG` workflow, the command processor, and OTA are
shared unchanged.

## How input works
The device is a BLE GATT server advertising a Nordic-UART-style service with:
- **RX** (write) — keystroke bytes from the laptop, fed into the current mode.
- **CTRL** (write) — control messages (`OPEN` palette, `CMD <x>`).
- **TX** (notify) — current device mode, shown by the companion.

`scripts/companion.py` (laptop side) captures keys with `pynput` and forwards
terminal-style byte sequences with `bleak`. In **terminal** mode the byte stream is
passed straight to SSH (arrows, Ctrl, Esc are their raw VT sequences — full
fidelity); in **notepad**/**command** mode the device interprets printable chars,
Enter, Backspace, and arrow keys.

## Flash the firmware
```bash
pio run -e rt          # build + upload (USB first time, OTA when reachable)
pio run -e rtdebug     # debug variant (serial automation protocol)
```
`rt` / `rtdebug` are short aliases for the `reterminal` / `reterminal-debug` envs that
**upload by default** (no `-t upload`). Use the full names to compile without flashing.
Switch devices by switching the env: `pio run -e td` (T-Deck) ↔ `pio run -e rt` (reTerminal).
No dedicated board package exists for the E1001; the build uses `boards/reTerminal-E1001.json`
(generic ESP32-S3, 32 MB flash, octal PSRAM).

## SD `/CONFIG`
Same format as the T-Deck (see the main README). For the companion you mainly need
`# wifi` and `# ssh`. Two notes:
- `# bt` line 1 sets the **advertised BLE name** (default `s-term-rt`). The companion
  matches this name.
- `# ota` line 1 should be a **unique mDNS host** (e.g. `reterm`) if a T-Deck is also
  on your LAN, so OTA targets the right device. Push with `STERM_OTA_HOST=reterm.local`.

## Run the companion (macOS)
```bash
uv sync                       # installs bleak + pynput
uv run scripts/companion.py   # connects to "s-term-rt"
uv run scripts/companion.py --list      # scan/list nearby BLE devices
uv run scripts/companion.py --name foo --toggle-key j --palette-key o
```
One-time macOS permissions — grant your terminal app BOTH:
- System Settings → Privacy & Security → **Accessibility**
- System Settings → Privacy & Security → **Input Monitoring**

On first write the device pairs ("just works"); accept any pairing prompt.

### Hotkeys
- **Cmd+Shift+K** — toggle forwarding. ON = every key goes to the device (suppressed
  locally); OFF = normal local typing.
- **Cmd+Shift+P** — open the device command palette. Then (with forwarding on) type a
  command like `ssh`, `np`, `daily`, `ls`, `s`.

Typical flow: press Cmd+Shift+P → type `ssh` Enter (device connects + switches to
terminal) → press Cmd+Shift+K and just type — your keystrokes drive the remote shell.
Cmd+Shift+K again to return to your laptop.

## Device buttons (standalone, no laptop)
- **Button 1 (Refresh, GPIO3)** — force a clean full redraw (clears e-ink ghosting).
- **Button 2 (GPIO4)** — open / close the command palette; also wakes from deep sleep.
- **Button 3 (GPIO5)** — toggle notepad ↔ terminal.

## Bring-up / testing without the laptop tool
Flash the debug build and use the existing serial automation protocol over USB
(see the main README "Serial protocol"):
```bash
pio run -e rtdebug
uv run scripts/tdeck_agent.py --boot-wait 2 "PING" "STATE"
uv run scripts/tdeck_agent.py "@I2CSCAN"          # expect 0x51 (RTC) 0x44 (SHT4x) 0x6B (charger)
uv run scripts/tdeck_agent.py "TEXT hello" "RENDER" "WAIT 4000" "STATE"
```
`scripts/agent_smoke.py` also works (point a camera at the 7.5″ panel).

## Display notes
- Layout is the 6×8 font at 800×480 → 133 columns × 58 rows (a real ≥80-col terminal).
  Change density in `src/firmware/layout.h`.
- 7.5″ e-paper is slow (full refresh ~2–4 s). Typing is coalesced — a burst of keys
  renders once, not per character. The Refresh button forces a clean redraw to clear
  ghosting. Heavy full-screen TUI output over SSH will visibly lag.

## What's different from the T-Deck build
| Area | T-Deck Pro | reTerminal E1001 |
|------|-----------|------------------|
| Input | physical keyboard + touch | laptop keystrokes over BLE |
| BLE role | HID keyboard/mouse (sends) | GATT input server (receives) |
| Display | 3.1″ 240×320 GDEQ031T10 | 7.5″ 800×480 GDEY075T7 |
| Radios | GPS / 4G / Meshtastic / LoRa | none (compiled out) |
| Battery | BQ27220 fuel gauge | ADC divider (GPIO1, enable GPIO21) |

Capability flags live in `src/firmware/device.h`; pins in `src/firmware/pins.h`.
