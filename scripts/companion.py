#!/usr/bin/env python3
"""Laptop companion for the s-term reTerminal build.

Connects to the device over BLE (Nordic-UART-style service) and forwards your
laptop's keystrokes to it. A global hotkey toggles "forwarding": while on, every
key is captured and sent to the device (and suppressed locally); while off, you
type into your laptop normally. A second hotkey opens the device command palette.

    uv run scripts/companion.py                 # connect to "s-term-rt", default hotkeys
    uv run scripts/companion.py --list          # scan and list nearby BLE devices
    uv run scripts/companion.py --name my-name   # match a different advertised name
    uv run scripts/companion.py --address <UUID> # connect to a specific device

Default hotkeys (configurable):
    Cmd+Shift+K   toggle forwarding on/off
    Cmd+Shift+P   open the device command palette (then type e.g. `ssh`, `np`)

macOS permissions (one-time): grant your terminal app BOTH
  System Settings -> Privacy & Security -> Accessibility
  System Settings -> Privacy & Security -> Input Monitoring
The first time the device receives a write it will pair ("just works"); accept
any pairing prompt.

Dependencies (already in pyproject): bleak, pynput.  Run via `uv run`.
"""

import argparse
import asyncio
import sys

from bleak import BleakClient, BleakScanner
from pynput import keyboard

# Nordic UART Service + a control characteristic (must match ble_input_module.hpp).
SVC_UUID  = "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
RX_UUID   = "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  # laptop -> device keystrokes
TX_UUID   = "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  # device -> laptop status (notify)
CTRL_UUID = "6e400004-b5a3-f393-e0a9-e50e24dcca9e"  # laptop -> device control


# --- Key -> terminal byte translation -------------------------------------

_SPECIAL = {
    keyboard.Key.enter: b"\r",
    keyboard.Key.backspace: b"\x7f",
    keyboard.Key.tab: b"\t",
    keyboard.Key.esc: b"\x1b",
    keyboard.Key.space: b" ",
    keyboard.Key.up: b"\x1b[A",
    keyboard.Key.down: b"\x1b[B",
    keyboard.Key.right: b"\x1b[C",
    keyboard.Key.left: b"\x1b[D",
    keyboard.Key.home: b"\x1b[H",
    keyboard.Key.end: b"\x1b[F",
    keyboard.Key.page_up: b"\x1b[5~",
    keyboard.Key.page_down: b"\x1b[6~",
    keyboard.Key.delete: b"\x1b[3~",
}

_MOD_KEYS = {
    keyboard.Key.cmd, keyboard.Key.cmd_l, keyboard.Key.cmd_r,
    keyboard.Key.shift, keyboard.Key.shift_l, keyboard.Key.shift_r,
    keyboard.Key.ctrl, keyboard.Key.ctrl_l, keyboard.Key.ctrl_r,
    keyboard.Key.alt, keyboard.Key.alt_l, keyboard.Key.alt_r,
}


def key_to_bytes(key, ctrl: bool, alt: bool):
    """Translate a pynput key (with modifier state) to a terminal byte sequence."""
    if key in _SPECIAL:
        return _SPECIAL[key]

    ch = getattr(key, "char", None)
    if ch is None:
        return None

    if ctrl:
        low = ch.lower()
        if "a" <= low <= "z":
            return bytes([ord(low) - ord("a") + 1])  # Ctrl-A..Ctrl-Z -> 0x01..0x1a
        if ch == " ":
            return b"\x00"
        return None  # other ctrl combos: ignore

    data = ch.encode("utf-8", "ignore")
    if alt and data:  # Option/Meta -> ESC prefix (xterm meta)
        return b"\x1b" + data
    return data or None


# --- Companion controller -------------------------------------------------

class Companion:
    def __init__(self, client, loop, toggle_key, palette_key):
        self.client = client
        self.loop = loop
        self.toggle_key = toggle_key.lower()
        self.palette_key = palette_key.lower()
        self.queue: asyncio.Queue = asyncio.Queue()
        self.forwarding = False
        self.cmd = self.shift = self.ctrl = self.alt = False

    # -- pynput thread side --
    def _set_mod(self, key, down):
        if key in (keyboard.Key.cmd, keyboard.Key.cmd_l, keyboard.Key.cmd_r):
            self.cmd = down
        elif key in (keyboard.Key.shift, keyboard.Key.shift_l, keyboard.Key.shift_r):
            self.shift = down
        elif key in (keyboard.Key.ctrl, keyboard.Key.ctrl_l, keyboard.Key.ctrl_r):
            self.ctrl = down
        elif key in (keyboard.Key.alt, keyboard.Key.alt_l, keyboard.Key.alt_r):
            self.alt = down

    def _post(self, item):
        self.loop.call_soon_threadsafe(self.queue.put_nowait, item)

    def intercept(self, event_type, event):
        # macOS: returning None suppresses the event system-wide.
        return None if self.forwarding else event

    def on_press(self, key):
        self._set_mod(key, True)
        if key in _MOD_KEYS:
            return

        ch = getattr(key, "char", None)
        if self.cmd and self.shift and ch:
            c = ch.lower()
            if c == self.toggle_key:
                self.forwarding = not self.forwarding
                state = "ON  (keys -> device)" if self.forwarding else "OFF (local typing)"
                print(f"\r[forwarding {state}]   ", flush=True)
                return
            if c == self.palette_key:
                self._post(("ctrl", "OPEN"))
                print("\r[device: open command palette]   ", flush=True)
                return

        if not self.forwarding:
            return
        if self.cmd:
            return  # don't forward Cmd-combos (Cmd has no terminal meaning)

        data = key_to_bytes(key, self.ctrl, self.alt)
        if data:
            self._post(("rx", data))

    def on_release(self, key):
        self._set_mod(key, False)

    # -- asyncio side --
    async def writer(self):
        while self.client.is_connected:
            kind, payload = await self.queue.get()
            try:
                if kind == "rx":
                    # Coalesce any immediately-queued keystrokes into one write.
                    chunk = payload
                    while not self.queue.empty():
                        k2, p2 = self.queue.get_nowait()
                        if k2 == "rx":
                            chunk += p2
                        else:
                            await self._write_ctrl(p2)
                    await self.client.write_gatt_char(RX_UUID, chunk, response=False)
                else:
                    await self._write_ctrl(payload)
            except Exception as e:  # noqa: BLE001 — keep the loop alive on transient errors
                print(f"\n[write error: {e}]", file=sys.stderr)

    async def _write_ctrl(self, text):
        await self.client.write_gatt_char(CTRL_UUID, text.encode(), response=True)


def on_status(_handle, data: bytearray):
    try:
        print(f"\r[device mode: {data.decode(errors='replace')}]   ", flush=True)
    except Exception:  # noqa: BLE001
        pass


# --- BLE plumbing ---------------------------------------------------------

async def find_device(args):
    if args.address:
        print(f"Connecting to {args.address} ...")
        return args.address
    print(f"Scanning for a device named '{args.name}' ...")
    dev = await BleakScanner.find_device_by_filter(
        lambda d, ad: (d.name or "").lower() == args.name.lower()
        or SVC_UUID.lower() in [u.lower() for u in (ad.service_uuids or [])],
        timeout=12.0,
    )
    if dev is None:
        print(f"No device matching '{args.name}' found. Is BLE on and the device awake?")
        print("Tip: run with --list to see what's nearby.")
        sys.exit(1)
    print(f"Found {dev.name or '?'} [{dev.address}]")
    return dev


async def list_devices():
    print("Scanning 8s ...")
    found = await BleakScanner.discover(timeout=8.0, return_adv=True)
    for addr, (dev, adv) in sorted(found.items(), key=lambda kv: -(kv[1][1].rssi or -999)):
        name = dev.name or adv.local_name or "?"
        print(f"  {adv.rssi:>4} dBm  {addr}  {name}")


async def run(args):
    if args.list:
        await list_devices()
        return

    target = await find_device(args)
    loop = asyncio.get_running_loop()

    async with BleakClient(target) as client:
        print("Connected. Pairing happens on first write — accept any prompt.")
        comp = Companion(client, loop, args.toggle_key, args.palette_key)

        try:
            await client.start_notify(TX_UUID, on_status)
        except Exception:  # noqa: BLE001 — notify is optional
            pass

        listener = keyboard.Listener(
            on_press=comp.on_press,
            on_release=comp.on_release,
            darwin_intercept=comp.intercept,
            suppress=False,
        )
        listener.start()

        print(f"Ready. {args.toggle_key.upper()}=toggle (Cmd+Shift), "
              f"{args.palette_key.upper()}=palette. Ctrl-C here to quit.")
        try:
            await comp.writer()
        finally:
            listener.stop()
    print("Disconnected.")


def main():
    ap = argparse.ArgumentParser(description="BLE keyboard companion for s-term reTerminal.")
    ap.add_argument("--name", default="s-term-rt", help="advertised device name to match")
    ap.add_argument("--address", help="connect to a specific BLE address/UUID")
    ap.add_argument("--list", action="store_true", help="scan and list nearby BLE devices")
    ap.add_argument("--toggle-key", default="k", dest="toggle_key",
                    help="letter for the Cmd+Shift+<key> forwarding toggle (default k)")
    ap.add_argument("--palette-key", default="p", dest="palette_key",
                    help="letter for the Cmd+Shift+<key> command palette (default p)")
    args = ap.parse_args()

    try:
        asyncio.run(run(args))
    except KeyboardInterrupt:
        print("\nBye.")


if __name__ == "__main__":
    main()
