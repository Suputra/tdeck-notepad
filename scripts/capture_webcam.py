#!/usr/bin/env python3
"""Capture still images or short videos from a camera source for T-Deck test artifacts."""

from __future__ import annotations

import argparse
import time
from urllib.parse import urlparse

try:
    import cv2
except ImportError as exc:  # pragma: no cover - import error path
    raise SystemExit(
        "opencv-python is required. Run: uv sync"
    ) from exc

DEFAULT_CAMERA_SOURCE = "http://10.0.44.199:4747/"


def parse_capture_source(source: str) -> int | str:
    text = (source or "").strip()
    if text.isdigit():
        return int(text)
    return text


def source_label(source: int | str) -> str:
    return f"device {source}" if isinstance(source, int) else source


def source_candidates(source: int | str) -> list[int | str]:
    if isinstance(source, int):
        return [source]
    parsed = urlparse(source)
    if parsed.scheme in {"http", "https"} and parsed.path in {"", "/"}:
        return [source, source.rstrip("/") + "/video"]
    return [source]


def open_capture(
    source: int | str,
    retries: int = 3,
    retry_delay_s: float = 0.75,
) -> tuple[cv2.VideoCapture | None, int | str]:
    candidates = source_candidates(source)
    total_attempts = max(retries, 0) + 1
    for attempt in range(1, total_attempts + 1):
        for candidate in candidates:
            cap = cv2.VideoCapture(candidate)
            if cap.isOpened():
                if candidate != source:
                    print(f"Opened fallback camera source: {source_label(candidate)}")
                return cap, candidate
            cap.release()
        if attempt < total_attempts:
            print(
                f"Open attempt {attempt}/{total_attempts} failed for {source_label(source)}; "
                f"retrying in {max(retry_delay_s, 0.0):.2f}s"
            )
            time.sleep(max(retry_delay_s, 0.0))
    return None, source


def capture_image(
    source: int | str,
    path: str,
    warmup_s: float,
    open_retries: int,
    open_retry_delay_s: float,
) -> int:
    cap, opened_source = open_capture(source, open_retries, open_retry_delay_s)
    if cap is None:
        print(f"Failed to open camera source: {source_label(source)}")
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
            print(f"Failed to read frame from camera source: {source_label(opened_source)}")
            return 3
        if not cv2.imwrite(path, frame):
            print(f"Failed to write image: {path}")
            return 4
        print(f"Wrote image: {path}")
        return 0
    finally:
        cap.release()


def capture_video(
    source: int | str,
    path: str,
    duration_s: float,
    warmup_s: float,
    fps: float,
    open_retries: int,
    open_retry_delay_s: float,
) -> int:
    cap, _opened_source = open_capture(source, open_retries, open_retry_delay_s)
    if cap is None:
        print(f"Failed to open camera source: {source_label(source)}")
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
    parser = argparse.ArgumentParser(description="Capture stills/videos for device validation.")
    parser.add_argument(
        "--source",
        default=DEFAULT_CAMERA_SOURCE,
        help="Camera source: URL (default) or numeric device index (e.g. 0)",
    )
    parser.add_argument(
        "--device",
        type=int,
        help="Deprecated webcam device index; overrides --source",
    )
    parser.add_argument("--warmup", type=float, default=1.0, help="Warm-up seconds before capture")
    parser.add_argument("--fps", type=float, default=15.0, help="Target FPS for video capture")
    parser.add_argument(
        "--open-retries",
        type=int,
        default=3,
        help="Retry count when opening camera source",
    )
    parser.add_argument(
        "--open-retry-delay",
        type=float,
        default=0.75,
        help="Seconds to wait between camera-open retries",
    )
    parser.add_argument("--image", help="Output image path (jpg/png)")
    parser.add_argument("--video", help="Output video path (mp4)")
    parser.add_argument("--duration", type=float, default=5.0, help="Video duration in seconds")
    args = parser.parse_args()

    if bool(args.image) == bool(args.video):
        print("Specify exactly one of --image or --video")
        return 1

    capture_source = args.device if args.device is not None else parse_capture_source(args.source)

    if args.image:
        return capture_image(
            capture_source,
            args.image,
            args.warmup,
            args.open_retries,
            args.open_retry_delay,
        )
    return capture_video(
        capture_source,
        args.video,
        args.duration,
        args.warmup,
        args.fps,
        args.open_retries,
        args.open_retry_delay,
    )


if __name__ == "__main__":
    raise SystemExit(main())
