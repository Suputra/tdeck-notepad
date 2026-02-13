#!/usr/bin/env python3
"""Capture still images or short videos from a webcam for T-Deck test artifacts."""

from __future__ import annotations

import argparse
import time

try:
    import cv2
except ImportError as exc:  # pragma: no cover - import error path
    raise SystemExit(
        "opencv-python is required. Run: uv sync"
    ) from exc


def capture_image(device: int, path: str, warmup_s: float) -> int:
    cap = cv2.VideoCapture(device)
    if not cap.isOpened():
        print(f"Failed to open webcam device {device}")
        return 2
    try:
        end = time.monotonic() + max(warmup_s, 0.0)
        frame = None
        while time.monotonic() < end:
            ok, frame = cap.read()
            if not ok:
                continue
        ok, frame = cap.read()
        if not ok or frame is None:
            print("Failed to read frame from webcam")
            return 3
        if not cv2.imwrite(path, frame):
            print(f"Failed to write image: {path}")
            return 4
        print(f"Wrote image: {path}")
        return 0
    finally:
        cap.release()


def capture_video(device: int, path: str, duration_s: float, warmup_s: float, fps: float) -> int:
    cap = cv2.VideoCapture(device)
    if not cap.isOpened():
        print(f"Failed to open webcam device {device}")
        return 2
    try:
        width = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH) or 1280)
        height = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT) or 720)
        fourcc = cv2.VideoWriter_fourcc(*"mp4v")
        out = cv2.VideoWriter(path, fourcc, fps, (width, height))
        if not out.isOpened():
            print(f"Failed to open video writer: {path}")
            return 3
        try:
            end_warmup = time.monotonic() + max(warmup_s, 0.0)
            while time.monotonic() < end_warmup:
                cap.read()

            end = time.monotonic() + max(duration_s, 0.0)
            while time.monotonic() < end:
                ok, frame = cap.read()
                if not ok:
                    continue
                out.write(frame)
            print(f"Wrote video: {path}")
            return 0
        finally:
            out.release()
    finally:
        cap.release()


def main() -> int:
    parser = argparse.ArgumentParser(description="Capture webcam stills/videos for device validation.")
    parser.add_argument("--device", type=int, default=0, help="Webcam device index")
    parser.add_argument("--warmup", type=float, default=1.0, help="Warm-up seconds before capture")
    parser.add_argument("--fps", type=float, default=15.0, help="Target FPS for video capture")
    parser.add_argument("--image", help="Output image path (jpg/png)")
    parser.add_argument("--video", help="Output video path (mp4)")
    parser.add_argument("--duration", type=float, default=5.0, help="Video duration in seconds")
    args = parser.parse_args()

    if bool(args.image) == bool(args.video):
        print("Specify exactly one of --image or --video")
        return 1

    if args.image:
        return capture_image(args.device, args.image, args.warmup)
    return capture_video(args.device, args.video, args.duration, args.warmup, args.fps)


if __name__ == "__main__":
    raise SystemExit(main())
