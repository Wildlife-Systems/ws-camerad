#!/usr/bin/env python3
"""
Demo Python Clip Client

Simple example of capturing a video clip from the camera daemon.

Usage:
  ./clip_client.py [start_offset] [end_offset]

Examples:
  ./clip_client.py -5 5    # 10 seconds: from 5s ago to 5s from now
  ./clip_client.py -10 0   # 10 seconds: from 10s ago to now (all from buffer)
  ./clip_client.py -3 2    # 5 seconds: from 3s ago to 2s from now
"""

import sys

from ws_camerad import capture_clip

DEFAULT_START_OFFSET = -5  # 5 seconds ago
DEFAULT_END_OFFSET = 5     # 5 seconds from now

def main():
    start_offset = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_START_OFFSET
    end_offset = int(sys.argv[2]) if len(sys.argv) > 2 else DEFAULT_END_OFFSET
    
    duration = end_offset - start_offset
    print(f"Capturing {duration}s clip (from {start_offset}s to {end_offset}s)...")
    if end_offset > 0:
        print("(Recording post-event footage, this may take a moment)")
    
    path = capture_clip(start_offset, end_offset)
    
    if path:
        print(f"Clip saved to: {path}")
        return 0
    else:
        return 1


if __name__ == "__main__":
    exit(main())
