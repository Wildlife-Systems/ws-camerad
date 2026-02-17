#!/usr/bin/env python3
"""
OpenCV Frame Analysis Example

This script reads BGR frames from the camera daemon's shared memory
and performs real-time analysis using OpenCV.

Features demonstrated:
- Motion detection
- Frame statistics (brightness, contrast)
- Edge detection

Requirements (Ubuntu packages):
    sudo apt install python3-opencv python3-numpy python3-ws-camerad

Usage:
    # First, enable BGR frame sharing in the daemon config:
    # enable_bgr_sharing = true
    
    # Then run:
    python3 opencv_analysis.py

    # Options:
    python3 opencv_analysis.py --show          # Show live preview window
    python3 opencv_analysis.py --motion        # Enable motion detection
    python3 opencv_analysis.py --stats         # Print frame statistics
    python3 opencv_analysis.py --save-motion   # Save frames on motion
"""

import argparse
import sys
import os
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

from ws_camerad import BGRFrameReader, DEFAULT_BGR_SHM_NAME


class MotionDetector:
    """Simple motion detection using frame differencing."""
    
    def __init__(self, threshold=25.0, min_area=500):
        self.threshold = threshold
        self.min_area = min_area
        self.prev_gray = None
    
    def detect(self, frame):
        """
        Detect motion in frame.
        
        Returns:
            Tuple of (motion_detected, bounding_boxes)
        """
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        gray = cv2.GaussianBlur(gray, (21, 21), 0)
        
        if self.prev_gray is None:
            self.prev_gray = gray
            return False, []
        
        # Compute absolute difference
        frame_diff = cv2.absdiff(self.prev_gray, gray)
        _, thresh = cv2.threshold(frame_diff, self.threshold, 255, cv2.THRESH_BINARY)
        
        # Dilate to fill gaps
        thresh = cv2.dilate(thresh, None, iterations=2)
        
        # Find contours
        contours, _ = cv2.findContours(thresh, cv2.RETR_EXTERNAL, cv2.CHAIN_APPROX_SIMPLE)
        
        bboxes = []
        motion_detected = False
        
        for contour in contours:
            if cv2.contourArea(contour) < self.min_area:
                continue
            
            motion_detected = True
            x, y, w, h = cv2.boundingRect(contour)
            bboxes.append((x, y, w, h))
        
        self.prev_gray = gray
        return motion_detected, bboxes


class FrameAnalyzer:
    """Computes frame statistics."""
    
    @staticmethod
    def compute_stats(frame):
        """Compute frame statistics."""
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        return {
            'brightness': np.mean(gray),
            'contrast': np.std(gray),
            'min': int(np.min(gray)),
            'max': int(np.max(gray))
        }
    
    @staticmethod
    def detect_edges(frame):
        """Detect edges using Canny."""
        gray = cv2.cvtColor(frame, cv2.COLOR_BGR2GRAY)
        return cv2.Canny(gray, 50, 150)


def draw_info(frame, info, fps):
    """Draw information overlay on frame."""
    overlay = frame.copy()
    cv2.rectangle(overlay, (10, 10), (300, 120), (0, 0, 0), -1)
    cv2.addWeighted(overlay, 0.5, frame, 0.5, 0, frame)
    
    y = 30
    cv2.putText(frame, "FPS: %.1f" % fps, (20, y), 
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2)
    y += 25
    cv2.putText(frame, "Brightness: %.1f" % info.get('brightness', 0), (20, y),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2)
    y += 25
    cv2.putText(frame, "Contrast: %.1f" % info.get('contrast', 0), (20, y),
                cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 255, 0), 2)
    y += 25
    if info.get('motion'):
        cv2.putText(frame, "MOTION DETECTED", (20, y),
                    cv2.FONT_HERSHEY_SIMPLEX, 0.6, (0, 0, 255), 2)


def main():
    parser = argparse.ArgumentParser(description='OpenCV Frame Analysis')
    parser.add_argument('--shm', default=DEFAULT_BGR_SHM_NAME, help='Shared memory name')
    parser.add_argument('--show', action='store_true', help='Show live preview')
    parser.add_argument('--motion', action='store_true', help='Enable motion detection')
    parser.add_argument('--stats', action='store_true', help='Print frame statistics')
    parser.add_argument('--edges', action='store_true', help='Show edge detection')
    parser.add_argument('--save-motion', type=str, default='', 
                        help='Directory to save frames on motion')
    parser.add_argument('--max-frames', type=int, default=0,
                        help='Maximum frames to process (0 = unlimited)')
    args = parser.parse_args()
    
    # Initialize components
    reader = BGRFrameReader(args.shm)
    motion_detector = MotionDetector() if args.motion or args.save_motion else None
    analyzer = FrameAnalyzer()
    
    if not reader.connect():
        return 1
    
    print("Connected: %dx%d" % (reader.width, reader.height))
    
    # Create save directory if needed
    if args.save_motion and not os.path.exists(args.save_motion):
        os.makedirs(args.save_motion)
    
    print("\nStarting analysis... Press Ctrl+C to stop.\n")
    
    frame_count = 0
    fps_start_time = time.time()
    fps_frame_count = 0
    current_fps = 0.0
    
    try:
        while True:
            # Read BGR frame directly - no conversion needed!
            bgr_frame, metadata = reader.read_frame(timeout_ms=100)
            
            if bgr_frame is None:
                continue
            
            # Update FPS counter
            fps_frame_count += 1
            elapsed = time.time() - fps_start_time
            if elapsed >= 1.0:
                current_fps = fps_frame_count / elapsed
                fps_frame_count = 0
                fps_start_time = time.time()
            
            # Compute statistics
            stats = {}
            if args.stats or args.show:
                stats = analyzer.compute_stats(bgr_frame)
            
            # Motion detection
            motion_detected = False
            motion_bboxes = []
            if motion_detector:
                motion_detected, motion_bboxes = motion_detector.detect(bgr_frame)
                stats['motion'] = motion_detected
            
            # Print stats
            if args.stats:
                print("Frame %d: brightness=%.1f, contrast=%.1f, motion=%s" % 
                      (frame_count, stats['brightness'], stats['contrast'], motion_detected))
            
            # Save on motion
            if args.save_motion and motion_detected:
                timestamp = datetime.now().strftime("%Y%m%d_%H%M%S_%f")
                filename = os.path.join(args.save_motion, "motion_" + timestamp + ".jpg")
                cv2.imwrite(filename, bgr_frame)
                print("Motion saved: " + filename)
            
            # Show preview
            if args.show:
                display_frame = bgr_frame.copy()
                
                # Draw motion bounding boxes
                for x, y, w, h in motion_bboxes:
                    cv2.rectangle(display_frame, (x, y), (x + w, y + h), (0, 255, 0), 2)
                
                # Draw info overlay
                draw_info(display_frame, stats, current_fps)
                
                # Show edge detection in separate window
                if args.edges:
                    edges = analyzer.detect_edges(bgr_frame)
                    cv2.imshow('Edges', edges)
                
                cv2.imshow('Camera Feed', display_frame)
                
                key = cv2.waitKey(1) & 0xFF
                if key == ord('q'):
                    break
                elif key == ord('s'):
                    filename = "snapshot_" + datetime.now().strftime('%Y%m%d_%H%M%S') + ".jpg"
                    cv2.imwrite(filename, bgr_frame)
                    print("Saved: " + filename)
            
            frame_count += 1
            
            if args.max_frames > 0 and frame_count >= args.max_frames:
                break
    
    except KeyboardInterrupt:
        print("\nStopping...")
    
    finally:
        reader.disconnect()
        if args.show:
            cv2.destroyAllWindows()
    
    print("\nProcessed %d frames" % frame_count)
    return 0


if __name__ == "__main__":
    sys.exit(main())
