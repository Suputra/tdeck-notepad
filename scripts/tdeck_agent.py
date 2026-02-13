#!/usr/bin/env python3
"""Send debug-agent commands to T-Deck firmware over serial."""

from __future__ import annotations

import argparse
import sys
import time
from typing import Optional

try:
    import serial
    from serial.tools import list_ports
except ImportError as exc:  # pragma: no cover - import error path
    raise SystemExit(
        "pyserial is required. Run: uv sync"
    ) from exc


def auto_detect_port() -> Optional[str]:
    preferred = []
    fallback = []
    for p in list_ports.comports():
        device = p.device or ""
        desc = (p.description or "").lower()
        hwid = (p.hwid or "").lower()
        if "usb" in desc or "usb" in hwid:
            preferred.append(device)
        else:
            fallback.append(device)
    if len(preferred) == 1:
        return preferred[0]
    if len(preferred) > 1:
        return None
    if len(fallback) == 1:
        return fallback[0]
    return None


def read_line(ser: serial.Serial, deadline: float) -> Optional[str]:
    buf = bytearray()
    while time.monotonic() < deadline:
        b = ser.read(1)
        if not b:
            continue
        if b == b"\r":
            continue
        if b == b"\n":
            return buf.decode("utf-8", errors="replace")
        buf.extend(b)
    return None


def send_and_wait(ser: serial.Serial, command: str, timeout_s: float, echo_all: bool) -> str:
    wire = command if command.startswith("@") else f"@{command}"
    ser.write((wire + "\n").encode("utf-8"))
    ser.flush()
    print(f">> {wire}")

    deadline = time.monotonic() + timeout_s
    while True:
        line = read_line(ser, deadline)
        if line is None:
            raise TimeoutError(f"timed out waiting for AGENT response to: {wire}")
        if line.startswith("AGENT "):
            print(f"<< {line}")
            return line
        if echo_all and line:
            print(f".. {line}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Send debug protocol commands to T-Deck firmware.")
    parser.add_argument(
        "--port",
        help="Serial port (default: auto-detect if exactly one USB-like port exists)",
    )
    parser.add_argument("--baud", type=int, default=115200, help="Serial baud rate")
    parser.add_argument("--timeout", type=float, default=4.0, help="Seconds to wait per command response")
    parser.add_argument(
        "--boot-wait",
        type=float,
        default=0.0,
        help="Seconds to read and print boot logs before sending commands",
    )
    parser.add_argument(
        "--echo-all",
        action="store_true",
        help="Print non-AGENT serial lines while waiting for responses",
    )
    parser.add_argument("commands", nargs="+", help="Commands like: PING, MODE, STATE, TEXT hello")
    args = parser.parse_args()

    port = args.port or auto_detect_port()
    if not port:
        print("Could not auto-detect a single serial port. Pass --port explicitly.", file=sys.stderr)
        return 2

    try:
        with serial.Serial(port=port, baudrate=args.baud, timeout=0.1) as ser:
            print(f"Connected to {port} @ {args.baud}")
            if args.boot_wait > 0:
                print(f"Reading boot logs for {args.boot_wait:.1f}s...")
                end = time.monotonic() + args.boot_wait
                while time.monotonic() < end:
                    line = read_line(ser, end)
                    if line:
                        print(f".. {line}")

            for cmd in args.commands:
                resp = send_and_wait(ser, cmd, args.timeout, args.echo_all)
                if resp.startswith("AGENT ERR"):
                    return 3
    except serial.SerialException as exc:
        print(f"Serial error: {exc}", file=sys.stderr)
        return 4
    except TimeoutError as exc:
        print(str(exc), file=sys.stderr)
        return 5

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
