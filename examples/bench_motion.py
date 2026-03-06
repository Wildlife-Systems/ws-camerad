#!/usr/bin/env python3
"""
Motion detection benchmark — measures per-frame detection time.

Modes:
  Mode A (default): BGR shared memory → cvtColor(BGR2GRAY) → downsample 4x → detect
  Mode B (--yuv):   Raw YUV shared memory → extract Y plane → downsample 4x → detect
  Mode C (--v4l2 N): V4L2 virtual camera (/dev/videoN) → cvtColor → detect
                     If daemon outputs pre-downsampled frames, use --no-downsample

Usage:
  python3 bench_motion.py               # Mode A: BGR path (Python native)
  python3 bench_motion.py --yuv         # Mode B: YUV Y-plane path (matches C++ native)
  python3 bench_motion.py --v4l2 30 --no-downsample  # Mode C: virtual camera
"""

import argparse
import sys
import time
import math
import signal

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

sys.path.insert(0, "/home/edwab/ws-camera-d/python")
from ws_camerad import BGRFrameReader, FrameSubscriber, DEFAULT_BGR_SHM_NAME

# The daemon creates /ws_camerad_frames (not /ws_camera_frames)
DEFAULT_RAW_SHM_NAME = "/ws_camerad_frames"

g_running = True

def signal_handler(sig, frame):
    global g_running
    g_running = False

signal.signal(signal.SIGINT, signal_handler)
signal.signal(signal.SIGTERM, signal_handler)


class MotionDetector:
    """Motion detection with downsampling — equivalent to C++ version."""

    def __init__(self, threshold=25.0, min_area=500, scale=4):
        self.threshold = threshold
        self.min_area = min_area
        self.scale = scale
        self.prev_blurred = None
        self.small_w = 0
        self.small_h = 0
        self.scaled_min_area = 0

    def init(self, w, h):
        self.small_w = w // self.scale
        self.small_h = h // self.scale
        self.scaled_min_area = self.min_area / (self.scale * self.scale)
        if self.scaled_min_area < 1:
            self.scaled_min_area = 1

    def detect(self, gray):
        """Detect motion from a grayscale frame. Returns bool."""
        # Downsample
        small = cv2.resize(gray, (self.small_w, self.small_h),
                           interpolation=cv2.INTER_NEAREST)
        blurred = cv2.GaussianBlur(small, (21, 21), 0)

        if self.prev_blurred is None:
            self.prev_blurred = blurred
            return False

        frame_diff = cv2.absdiff(self.prev_blurred, blurred)
        _, thresh = cv2.threshold(frame_diff, self.threshold, 255, cv2.THRESH_BINARY)
        thresh = cv2.dilate(thresh, None, iterations=2)
        contours, _ = cv2.findContours(thresh, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)

        motion = any(cv2.contourArea(c) >= self.scaled_min_area for c in contours)
        self.prev_blurred = blurred
        return motion


def compute_stats(times):
    """Compute statistics from a list of times in microseconds."""
    if not times:
        return {}
    times_sorted = sorted(times)
    n = len(times_sorted)
    mean = sum(times_sorted) / n
    median = times_sorted[n // 2]
    p95 = times_sorted[int(n * 0.95)]
    p99 = times_sorted[int(n * 0.99)]
    min_val = times_sorted[0]
    max_val = times_sorted[-1]
    variance = sum((t - mean) ** 2 for t in times_sorted) / n
    stddev = math.sqrt(variance)
    return {
        'mean': mean, 'median': median, 'stddev': stddev,
        'p95': p95, 'p99': p99, 'min': min_val, 'max': max_val
    }


def main():
    parser = argparse.ArgumentParser(description="Motion detection benchmark (Python)")
    parser.add_argument("--yuv", action="store_true",
                        help="Use raw YUV shared memory (Y plane) instead of BGR")
    parser.add_argument("--v4l2", type=int, default=-1, metavar="N",
                        help="Use V4L2 virtual camera /dev/videoN")
    parser.add_argument("--no-downsample", action="store_true",
                        help="Process at full resolution (no downsampling)")
    parser.add_argument("--frames", type=int, default=300,
                        help="Number of frames to benchmark (default: 300)")
    parser.add_argument("--threshold", type=float, default=25.0)
    parser.add_argument("--min-area", type=int, default=500)
    parser.add_argument("--scale", type=int, default=4)
    args = parser.parse_args()

    scale = 1 if args.no_downsample else args.scale

    read_times = []
    detect_times = []
    total_times = []
    cvt_times = []  # BGR→gray conversion time (Mode A only)
    motion_count = 0

    if args.v4l2 >= 0:
        # Mode C: V4L2 virtual camera (daemon-side downsampled)
        dev = f"/dev/video{args.v4l2}"
        cap = cv2.VideoCapture(dev, cv2.CAP_V4L2)
        if not cap.isOpened():
            print(f"Failed to open V4L2 device: {dev}")
            return 1

        w = int(cap.get(cv2.CAP_PROP_FRAME_WIDTH))
        h = int(cap.get(cv2.CAP_PROP_FRAME_HEIGHT))
        detector = MotionDetector(args.threshold, args.min_area, scale)
        detector.init(w, h)

        print(f"=== Python Motion Benchmark (V4L2 virtual camera: {dev}) ===")
        print(f"Resolution: {w}x{h} → {w//scale}x{h//scale}")
        print(f"Frames: {args.frames}")
        print(f"Threshold: {args.threshold}, MinArea: {args.min_area}")
        print(f"Scale: {scale}x\n")

        i = 0
        while i < args.frames and g_running:
            t0 = time.perf_counter()
            ret, frame_bgr = cap.read()
            if not ret or frame_bgr is None:
                continue
            t1 = time.perf_counter()

            gray = cv2.cvtColor(frame_bgr, cv2.COLOR_BGR2GRAY)
            t1b = time.perf_counter()
            motion = detector.detect(gray)
            t2 = time.perf_counter()

            if motion:
                motion_count += 1

            read_times.append((t1 - t0) * 1e6)
            cvt_times.append((t1b - t1) * 1e6)
            detect_times.append((t2 - t1b) * 1e6)
            total_times.append((t2 - t0) * 1e6)
            i += 1

        cap.release()

    elif args.yuv:
        # Mode B: Raw YUV Y-plane (equivalent to C++ native path)
        reader = FrameSubscriber(DEFAULT_RAW_SHM_NAME)
        if not reader.connect():
            print("Failed to connect to raw shared memory.")
            return 1

        w, h = reader.width, reader.height
        detector = MotionDetector(args.threshold, args.min_area, scale)
        detector.init(w, h)

        print(f"=== Python Motion Benchmark (YUV Y-plane path) ===")
        print(f"Resolution: {w}x{h} → {w//scale}x{h//scale}")
        print(f"Frames: {args.frames}")
        print(f"Threshold: {args.threshold}, MinArea: {args.min_area}")
        print(f"Scale: {scale}x\n")

        i = 0
        while i < args.frames and g_running:
            t0 = time.perf_counter()
            frame_data, meta = reader.read_frame(timeout_ms=100)
            if frame_data is None:
                continue
            t1 = time.perf_counter()

            # Extract Y plane (first w*h bytes of NV12)
            y_plane = frame_data[:w * h].reshape((h, w))
            motion = detector.detect(y_plane)
            t2 = time.perf_counter()

            if motion:
                motion_count += 1

            read_times.append((t1 - t0) * 1e6)
            detect_times.append((t2 - t1) * 1e6)
            total_times.append((t2 - t0) * 1e6)
            i += 1

        reader.disconnect()

    else:
        # Mode A: BGR path (Python native path)
        reader = BGRFrameReader()
        if not reader.connect():
            print("Failed to connect to BGR shared memory.")
            return 1

        w, h = reader.width, reader.height
        detector = MotionDetector(args.threshold, args.min_area, scale)
        detector.init(w, h)

        print(f"=== Python Motion Benchmark (BGR path) ===")
        print(f"Resolution: {w}x{h} → {w//scale}x{h//scale}")
        print(f"Frames: {args.frames}")
        print(f"Threshold: {args.threshold}, MinArea: {args.min_area}")
        print(f"Scale: {scale}x\n")

        i = 0
        while i < args.frames and g_running:
            t0 = time.perf_counter()
            bgr_frame, meta = reader.read_frame(timeout_ms=100)
            if bgr_frame is None:
                continue
            t1 = time.perf_counter()

            # BGR → grayscale (this is the extra cost vs C++ YUV path)
            gray = cv2.cvtColor(bgr_frame, cv2.COLOR_BGR2GRAY)
            t1b = time.perf_counter()

            motion = detector.detect(gray)
            t2 = time.perf_counter()

            if motion:
                motion_count += 1

            read_times.append((t1 - t0) * 1e6)
            cvt_times.append((t1b - t1) * 1e6)
            detect_times.append((t2 - t1b) * 1e6)
            total_times.append((t2 - t0) * 1e6)
            i += 1

        reader.disconnect()

    # Print results
    read_s = compute_stats(read_times)
    det_s = compute_stats(detect_times)
    tot_s = compute_stats(total_times)
    cvt_s = compute_stats(cvt_times) if cvt_times else None

    n = len(total_times)
    print(f"\nResults ({n} frames, {motion_count} motion):")
    header = f"  {'Phase':<18} {'Mean':>8} {'Median':>8} {'StdDev':>8} {'P95':>8} {'Min':>8} {'Max':>8}"
    units  = f"  {'':<18} {'(µs)':>8} {'(µs)':>8} {'(µs)':>8} {'(µs)':>8} {'(µs)':>8} {'(µs)':>8}"
    print(header)
    print(units)
    print(f"  {'-' * 70}")

    def print_row(label, s):
        print(f"  {label:<18} {s['mean']:8.0f} {s['median']:8.0f} {s['stddev']:8.0f} "
              f"{s['p95']:8.0f} {s['min']:8.0f} {s['max']:8.0f}")

    print_row("Frame read", read_s)
    if cvt_s:
        print_row("BGR→Gray", cvt_s)
    print_row("Detection", det_s)
    print_row("Total", tot_s)

    print(f"\n  Throughput: {1e6 / det_s['mean']:.1f} fps (detection only)")
    print(f"  Throughput: {1e6 / tot_s['mean']:.1f} fps (total incl. read)")

    return 0


if __name__ == "__main__":
    sys.exit(main())
