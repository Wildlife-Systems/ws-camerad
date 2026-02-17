#!/usr/bin/env python3
"""
Demo Python Still Client

Simple example of capturing a still image from the camera daemon.

Usage:
  ./still_client.py [time_offset]

Examples:
  ./still_client.py       # Capture now
  ./still_client.py -2    # Capture from 2 seconds ago
  ./still_client.py 1     # Capture 1 second in the future
"""

import sys

from ws_camerad import capture_still

def main():
    time_offset = int(sys.argv[1]) if len(sys.argv) > 1 else 0
    
    if time_offset == 0:
        print("Capturing still image...")
    elif time_offset < 0:
        print(f"Capturing still from {-time_offset}s ago...")
    else:
        print(f"Capturing still {time_offset}s from now...")
    
    path = capture_still(time_offset)
    
    if path:
        print(f"Still saved to: {path}")
        return 0
    else:
        return 1


if __name__ == "__main__":
    exit(main())
