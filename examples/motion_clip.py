#!/usr/bin/env python3
"""
Motion-triggered clip capture.

Reads BGR frames from ws-camerad shared memory, runs motion detection,
and triggers a 4-second clip (-2s to +2s) on each motion event.
A 2-second cooldown prevents back-to-back triggers.

Requirements:
    sudo apt install python3-opencv python3-numpy python3-ws-camerad

Usage:
    python3 motion_clip.py
    python3 motion_clip.py --threshold 30 --min-area 800
"""

import argparse
import sys
import time
from datetime import datetime

try:
    import numpy as np
except ImportError:
    print("NumPy not found. Install with: sudo apt install python3-numpy")
    sys.exit(1)

try:
    import cv2
except ImportError:
    print("OpenCV not found. Install with: sudo apt install python3-opencv")
    sys.exit(1)

from ws_camerad import BGRFrameReader, capture_clip, DEFAULT_BGR_SHM_NAME


class MotionDetector:
    """Simple motion detection using frame differencing."""

    def __init__(self, threshold=25.0, min_area=500):
        self.threshold = threshold
        self.min_area = min_area
        self.prev_gray = None

    def detect(self, frame):
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        gray = cv2.GaussianBlur(gray, (21, 21), 0)

        if self.prev_gray is None:
            self.prev_gray = gray
            return False

        frame_diff = cv2.absdiff(self.prev_gray, gray)
        _, thresh = cv2.threshold(frame_diff, self.threshold, 255, cv2.THRESH_BINARY)
        thresh = cv2.dilate(thresh, None, iterations=2)
        contours, _ = cv2.findContours(thresh, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)

        motion = any(cv2.contourArea(c) >= self.min_area for c in contours)
        self.prev_gray = gray
        return motion


def main():
    parser = argparse.ArgumentParser(description="Motion-triggered clip capture")
    parser.add_argument("--shm", default=DEFAULT_BGR_SHM_NAME, help="BGR shared memory name")
    parser.add_argument("--threshold", type=float, default=25.0, help="Motion threshold (default: 25)")
    parser.add_argument("--min-area", type=int, default=500, help="Min contour area (default: 500)")
    parser.add_argument("--cooldown", type=float, default=2.0, help="Cooldown between triggers in seconds (default: 2)")
    parser.add_argument("--pre", type=int, default=-2, help="Seconds before event (default: -2)")
    parser.add_argument("--post", type=int, default=2, help="Seconds after event (default: 2)")
    args = parser.parse_args()

    reader = BGRFrameReader(args.shm)
    detector = MotionDetector(threshold=args.threshold, min_area=args.min_area)

    if not reader.connect():
        print("Failed to connect to shared memory. Is ws-camerad running with enable_bgr_sharing=true?")
        return 1

    print(f"Connected: {reader.width}x{reader.height}")
    print(f"Motion detection active (threshold={args.threshold}, min_area={args.min_area})")
    print(f"Clip window: {args.pre}s to +{args.post}s ({args.post - args.pre}s total)")
    print(f"Cooldown: {args.cooldown}s")
    print("Watching for motion... Press Ctrl+C to stop.\n")

    last_trigger = 0.0

    try:
        while True:
            bgr_frame, metadata = reader.read_frame(timeout_ms=100)
            if bgr_frame is None:
                continue

            if not detector.detect(bgr_frame):
                continue

            now = time.time()
            if now - last_trigger < args.cooldown:
                continue

            last_trigger = now
            ts = datetime.now().strftime("%H:%M:%S")
            print(f"[{ts}] Motion detected — capturing clip...", end=" ", flush=True)

            path = capture_clip(args.pre, args.post)
            if path:
                print(f"saved: {path}")
            else:
                print("clip capture failed")

    except KeyboardInterrupt:
        print("\nStopping.")
    finally:
        reader.disconnect()

    return 0


if __name__ == "__main__":
    sys.exit(main())
