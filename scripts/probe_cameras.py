#!/usr/bin/env python3
"""Probe webcam device indices and report basic frame quality metrics."""

from __future__ import annotations

import argparse
import time

try:
    import cv2
except ImportError as exc:  # pragma: no cover - import error path
    raise SystemExit("opencv-python is required. Run: uv sync") from exc


def probe_device(index: int, warmup_s: float) -> tuple[bool, bool, float, float, float]:
    cap = cv2.VideoCapture(index)
    if not cap.isOpened():
        return False, False, 0.0, 0.0, 0.0

    try:
        end = time.monotonic() + max(warmup_s, 0.0)
        frame = None
        while time.monotonic() < end:
            ok, f = cap.read()
            if ok:
                frame = f

        ok, f = cap.read()
        if ok:
            frame = f

        if frame is None:
            return True, False, 0.0, 0.0, 0.0

        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        mean = float(gray.mean())
        std = float(gray.std())
        dark_pct = float((gray < 10).mean() * 100.0)
        return True, True, mean, std, dark_pct
    finally:
        cap.release()


def main() -> int:
    parser = argparse.ArgumentParser(description="Probe webcam indices and frame quality.")
    parser.add_argument("--max-index", type=int, default=5, help="Highest device index to probe")
    parser.add_argument("--warmup", type=float, default=1.5, help="Warmup seconds per device")
    args = parser.parse_args()

    any_usable = False
    print("index\topened\tframe\tmean\tstd\tdark_pct(<10)")
    for idx in range(args.max_index + 1):
        opened, has_frame, mean, std, dark_pct = probe_device(idx, args.warmup)
        print(
            f"{idx}\t{int(opened)}\t{int(has_frame)}\t"
            f"{mean:.2f}\t{std:.2f}\t{dark_pct:.2f}"
        )
        if opened and has_frame and mean > 5.0 and std > 2.0:
            any_usable = True

    if not any_usable:
        print("No usable camera feed found.")
        return 2

    print("At least one usable camera feed detected.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
