#!/usr/bin/env python3
"""
Camera Daemon Python Client Library

This module provides a Python interface for interacting with the camera daemon.
"""

import socket
import struct
import mmap
import os
import time
import json
from dataclasses import dataclass
from typing import Optional, Tuple, Any
import numpy as np


# Default paths
DEFAULT_SOCKET_PATH = "/run/ws-camerad/control.sock"
DEFAULT_SHM_NAME = "/ws_camera_frames"
DEFAULT_BGR_SHM_NAME = "/ws_camera_frames_bgr"
HEADER_SIZE = 64


@dataclass
class Response:
    """Response from the daemon (JSON format).
    
    Response format:
        Success: {"ok": true, "path": "..."}  or  {"ok": true, "data": {...}}
        Error:   {"ok": false, "error": "..."}
    """
    success: bool
    path: Optional[str] = None
    data: Optional[Any] = None
    error: Optional[str] = None
    raw: str = ""
    
    @classmethod
    def from_string(cls, s: str) -> 'Response':
        s = s.strip()
        try:
            obj = json.loads(s)
            return cls(
                success=obj.get("ok", False),
                path=obj.get("path"),
                data=obj.get("data"),
                error=obj.get("error"),
                raw=s
            )
        except json.JSONDecodeError:
            # Fallback for malformed responses
            return cls(success=False, error=f"Invalid JSON: {s}", raw=s)


class CameraClient:
    """
    Client for communicating with the camera daemon via control socket.
    
    Example:
        with CameraClient() as client:
            # Capture a still image
            response = client.capture_still()
            if response.success:
                print(f"Image saved to: {response.path}")
            
            # Capture a clip (5s before to 5s after = 10s)
            response = client.capture_clip(-5, 5)
            if response.success:
                print(f"Clip saved to: {response.path}")
            
            # Get status
            status = client.get_status()
            if status.success:
                print(status.data)
    """
    
    def __init__(self, socket_path: str = DEFAULT_SOCKET_PATH, timeout: float = 60.0):
        """
        Initialize camera client.
        
        Args:
            socket_path: Path to the daemon control socket
            timeout: Socket timeout in seconds (for clip operations)
        """
        self.socket_path = socket_path
        self.timeout = timeout
        self._sock: Optional[socket.socket] = None
    
    def connect(self) -> bool:
        """Connect to the camera daemon."""
        try:
            self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self._sock.settimeout(self.timeout)
            self._sock.connect(self.socket_path)
            return True
        except FileNotFoundError:
            print(f"Socket not found: {self.socket_path}")
            print("Is the camera daemon running?")
            self._sock = None
            return False
        except ConnectionRefusedError:
            print(f"Connection refused: {self.socket_path}")
            print("Is the camera daemon running?")
            self._sock = None
            return False
        except Exception as e:
            print(f"Connection failed: {e}")
            self._sock = None
            return False
    
    def disconnect(self):
        """Disconnect from the daemon."""
        if self._sock:
            self._sock.close()
            self._sock = None
    
    def is_connected(self) -> bool:
        """Check if connected to the daemon."""
        return self._sock is not None
    
    def send_command(self, command: str, timeout: Optional[float] = None) -> Response:
        """Send a command and receive response.
        
        Args:
            command: Command string to send
            timeout: Override socket timeout for this command (seconds)
        """
        if not self._sock:
            if not self.connect():
                return Response(success=False, error="Not connected")
        
        try:
            # Set timeout if specified
            if timeout is not None:
                self._sock.settimeout(timeout)
            
            # Send command
            if not command.endswith('\n'):
                command += '\n'
            self._sock.sendall(command.encode())
            
            # Receive response
            response = self._sock.recv(8192).decode().strip()
            return Response.from_string(response)
        except socket.timeout:
            return Response(success=False, error="Timeout waiting for response")
        except Exception as e:
            return Response(success=False, error=str(e))
        finally:
            # Restore default timeout
            if timeout is not None and self._sock:
                self._sock.settimeout(self.timeout)
    
    def capture_still(self, time_offset: int = 0) -> Response:
        """Capture a still image.
        
        Args:
            time_offset: Seconds offset (negative=past, positive=future, 0=now)
        """
        if time_offset == 0:
            return self.send_command("STILL")
        return self.send_command(f"STILL {time_offset}")
    
    def capture_clip(self, start_offset: int = -5, end_offset: int = 5) -> Response:
        """Capture a video clip.
        
        Args:
            start_offset: Start time relative to now (negative=past, positive=future)
            end_offset: End time relative to now (negative=past, positive=future)
        
        Examples:
            capture_clip(-5, 5)   # 10 seconds: from 5s ago to 5s from now
            capture_clip(-10, 0)  # 10 seconds: from 10s ago to now (all from buffer)
            capture_clip(-3, 2)   # 5 seconds: from 3s ago to 2s from now
        
        Returns:
            Response with .path containing the clip file path on success
        """
        duration = end_offset - start_offset
        # Allow extra time for post-event recording plus processing
        timeout = abs(start_offset) + max(0, end_offset) + 30
        return self.send_command(f"CLIP {start_offset} {end_offset}", timeout=timeout)
    
    def set_parameter(self, key: str, value: str) -> Response:
        """Set a camera parameter."""
        return self.send_command(f"SET {key} {value}")
    
    def get_status(self) -> Response:
        """Get daemon status."""
        return self.send_command("GET STATUS")
    
    def __enter__(self):
        self.connect()
        return self
    
    def __exit__(self, *args):
        self.disconnect()


class FrameSubscriber:
    """
    Subscriber for raw video frames via shared memory.
    
    Example:
        subscriber = FrameSubscriber()
        if subscriber.connect():
            print(f"Resolution: {subscriber.width}x{subscriber.height}")
            
            while True:
                frame, metadata = subscriber.read_frame(timeout_ms=1000)
                if frame is not None:
                    # Process frame with OpenCV, etc.
                    print(f"Got frame {metadata['sequence']}")
    """
    
    # Header format (must match C++ FramePublisher::Header)
    HEADER_FORMAT = "QIIIIIIQIxxxx"  # sequence, ready, w, h, stride, format, size, ts, keyframe, reserved
    HEADER_SIZE = struct.calcsize(HEADER_FORMAT) + 16  # Plus reserved bytes
    
    def __init__(self, shm_name: str = DEFAULT_SHM_NAME):
        self.shm_name = shm_name
        self._fd: Optional[int] = None
        self._mm: Optional[mmap.mmap] = None
        self._last_sequence: int = 0
        
        self.width: int = 0
        self.height: int = 0
        self.stride: int = 0
        self.format: int = 0
        self.frame_size: int = 0
    
    def connect(self) -> bool:
        """Connect to the shared memory."""
        try:
            # Open shared memory
            self._fd = os.open(f"/dev/shm{self.shm_name}", os.O_RDONLY)
            
            # Get size
            stat = os.fstat(self._fd)
            
            # Map memory
            self._mm = mmap.mmap(self._fd, stat.st_size, mmap.MAP_SHARED, mmap.PROT_READ)
            
            # Read header to get frame info
            self._read_header()
            
            return True
        except Exception as e:
            print(f"Failed to connect to shared memory: {e}")
            self.disconnect()
            return False
    
    def disconnect(self):
        """Disconnect from shared memory."""
        if self._mm:
            self._mm.close()
            self._mm = None
        if self._fd is not None:
            os.close(self._fd)
            self._fd = None
    
    def _read_header(self) -> dict:
        """Read the header from shared memory."""
        if not self._mm:
            return {}
        
        self._mm.seek(0)
        header_data = self._mm.read(64)  # Read enough for header
        
        # Parse header (simplified - actual format depends on atomic alignment)
        sequence = struct.unpack_from("Q", header_data, 0)[0]
        frame_ready = struct.unpack_from("I", header_data, 8)[0]
        self.width = struct.unpack_from("I", header_data, 12)[0]
        self.height = struct.unpack_from("I", header_data, 16)[0]
        self.stride = struct.unpack_from("I", header_data, 20)[0]
        self.format = struct.unpack_from("I", header_data, 24)[0]
        self.frame_size = struct.unpack_from("I", header_data, 28)[0]
        timestamp = struct.unpack_from("Q", header_data, 32)[0]
        is_keyframe = struct.unpack_from("I", header_data, 40)[0]
        
        return {
            'sequence': sequence,
            'frame_ready': frame_ready,
            'width': self.width,
            'height': self.height,
            'stride': self.stride,
            'format': self.format,
            'frame_size': self.frame_size,
            'timestamp_us': timestamp,
            'is_keyframe': bool(is_keyframe)
        }
    
    def read_frame(self, timeout_ms: int = -1) -> Tuple[Optional[np.ndarray], Optional[dict]]:
        """
        Read the latest frame from shared memory.
        
        Args:
            timeout_ms: Timeout in milliseconds (-1 = infinite, 0 = no wait)
            
        Returns:
            Tuple of (frame_data, metadata) or (None, None) on timeout
        """
        if not self._mm:
            return None, None
        
        start_time = time.time()
        
        while True:
            header = self._read_header()
            
            if header.get('sequence', 0) > self._last_sequence and header.get('frame_ready', 0):
                # New frame available
                self._last_sequence = header['sequence']
                
                # Read frame data
                self._mm.seek(64)  # Skip header (adjust based on actual header size)
                frame_bytes = self._mm.read(self.frame_size)
                
                # Convert to numpy array (NV12 format)
                frame = np.frombuffer(frame_bytes, dtype=np.uint8)
                
                return frame, header
            
            # Check timeout
            if timeout_ms == 0:
                return None, None
            
            if timeout_ms > 0:
                elapsed_ms = (time.time() - start_time) * 1000
                if elapsed_ms >= timeout_ms:
                    return None, None
            
            time.sleep(0.0001)  # 100us
    
    def has_new_frame(self) -> bool:
        """Check if a new frame is available."""
        if not self._mm:
            return False
        header = self._read_header()
        return header.get('sequence', 0) > self._last_sequence and header.get('frame_ready', 0)
    
    def __enter__(self):
        self.connect()
        return self
    
    def __exit__(self, *args):
        self.disconnect()


class BGRFrameReader:
    """
    Reader for BGR video frames via shared memory.
    Optimized for OpenCV - frames are (height, width, 3) BGR arrays.
    
    Requires: enable_bgr_sharing=true in daemon config
    """
    
    def __init__(self, shm_name: str = DEFAULT_BGR_SHM_NAME):
        self.shm_name = shm_name
        self._fd: Optional[int] = None
        self._mm: Optional[mmap.mmap] = None
        self._last_sequence: int = 0
        self.width: int = 0
        self.height: int = 0
        self.stride: int = 0
        self.frame_size: int = 0
    
    def connect(self) -> bool:
        """Connect to the shared memory."""
        try:
            shm_path = f"/dev/shm{self.shm_name}"
            if not os.path.exists(shm_path):
                print(f"Shared memory not found: {shm_path}")
                print("Make sure the daemon is running with 'enable_bgr_sharing = true'")
                return False
            
            self._fd = os.open(shm_path, os.O_RDONLY)
            stat = os.fstat(self._fd)
            self._mm = mmap.mmap(self._fd, stat.st_size, mmap.MAP_SHARED, mmap.PROT_READ)
            self._read_header()
            return True
        except Exception as e:
            print(f"Failed to connect: {e}")
            self.disconnect()
            return False
    
    def disconnect(self):
        """Disconnect from shared memory."""
        if self._mm:
            self._mm.close()
            self._mm = None
        if self._fd is not None:
            os.close(self._fd)
            self._fd = None
    
    def _read_header(self) -> dict:
        """Read the header from shared memory."""
        if not self._mm:
            return {}
        self._mm.seek(0)
        header_data = self._mm.read(HEADER_SIZE)
        sequence = struct.unpack_from("Q", header_data, 0)[0]
        frame_ready = struct.unpack_from("I", header_data, 8)[0]
        self.width = struct.unpack_from("I", header_data, 12)[0]
        self.height = struct.unpack_from("I", header_data, 16)[0]
        self.stride = struct.unpack_from("I", header_data, 20)[0]
        self.frame_size = struct.unpack_from("I", header_data, 28)[0]
        timestamp = struct.unpack_from("Q", header_data, 32)[0]
        return {'sequence': sequence, 'frame_ready': frame_ready, 'timestamp_us': timestamp}
    
    def read_frame(self, timeout_ms: int = 100) -> Tuple[Optional[np.ndarray], Optional[dict]]:
        """Read the latest BGR frame (height, width, 3) numpy array."""
        if not self._mm:
            return None, None
        start_time = time.time()
        while True:
            header = self._read_header()
            if header.get('sequence', 0) > self._last_sequence and header.get('frame_ready', 0):
                self._last_sequence = header['sequence']
                self._mm.seek(HEADER_SIZE)
                frame_bytes = self._mm.read(self.frame_size)
                frame = np.frombuffer(frame_bytes, dtype=np.uint8).copy()
                frame = frame.reshape((self.height, self.width, 3))
                return frame, header
            if timeout_ms == 0:
                return None, None
            if timeout_ms > 0:
                if (time.time() - start_time) * 1000 >= timeout_ms:
                    return None, None
            time.sleep(0.001)
    
    def __iter__(self):
        while True:
            frame, meta = self.read_frame(timeout_ms=1000)
            if frame is not None:
                yield frame, meta
    
    def __enter__(self):
        self.connect()
        return self
    
    def __exit__(self, *args):
        self.disconnect()


# Convenience functions

def capture_still(time_offset: int = 0, socket_path: str = DEFAULT_SOCKET_PATH) -> Optional[str]:
    """Capture a still image and return the file path.
    
    Args:
        time_offset: Seconds offset (negative=past, positive=future, 0=now)
        socket_path: Path to daemon socket
    
    Returns:
        File path on success, None on failure
    """
    with CameraClient(socket_path) as client:
        response = client.capture_still(time_offset)
        if response.success:
            return response.path
        else:
            print(f"Error: {response.error}")
            return None


def capture_clip(start_offset: int = -5, end_offset: int = 5, 
                 socket_path: str = DEFAULT_SOCKET_PATH) -> Optional[str]:
    """Capture a video clip and return the file path.
    
    Args:
        start_offset: Start time relative to now (negative=past, positive=future)
        end_offset: End time relative to now (negative=past, positive=future)
        socket_path: Path to daemon socket
    
    Examples:
        capture_clip(-5, 5)   # 10 seconds: from 5s ago to 5s from now
        capture_clip(-10, 0)  # 10 seconds from buffer only
    
    Returns:
        File path on success, None on failure
    """
    with CameraClient(socket_path) as client:
        response = client.capture_clip(start_offset, end_offset)
        if response.success:
            return response.path
        else:
            print(f"Error: {response.error}")
            return None


def get_status(socket_path: str = DEFAULT_SOCKET_PATH) -> Optional[dict]:
    """Get daemon status as a dictionary."""
    with CameraClient(socket_path) as client:
        response = client.get_status()
        if response.success:
            return response.data if response.data else {'raw': response.raw}
        return None


if __name__ == "__main__":
    import sys
    
    def print_usage():
        print("Camera Daemon Python Client")
        print()
        print("Usage:")
        print(f"  {sys.argv[0]} still [offset]            - Capture a still image")
        print(f"  {sys.argv[0]} clip <start> <end>        - Capture a video clip")
        print(f"  {sys.argv[0]} status                    - Get daemon status")
        print(f"  {sys.argv[0]} frames                    - Consume frames from shared memory")
        print()
        print("Examples:")
        print(f"  {sys.argv[0]} still           # Capture now")
        print(f"  {sys.argv[0]} still -2        # Capture from 2 seconds ago")
        print(f"  {sys.argv[0]} clip -5 5       # 10s clip: 5s before to 5s after now")
        print(f"  {sys.argv[0]} clip -10 0      # 10s clip from buffer only")
        print()
        print("Time offsets: negative=past, positive=future, 0=now")
    
    if len(sys.argv) < 2:
        print_usage()
        sys.exit(0)
    
    command = sys.argv[1].lower()
    
    if command == "still":
        offset = int(sys.argv[2]) if len(sys.argv) > 2 else 0
        path = capture_still(offset)
        if path:
            print(f"Still captured: {path}")
    
    elif command == "clip":
        if len(sys.argv) < 4:
            print("Error: clip requires start and end offsets")
            print(f"Usage: {sys.argv[0]} clip <start_offset> <end_offset>")
            print(f"Example: {sys.argv[0]} clip -5 5")
            sys.exit(1)
        start_offset = int(sys.argv[2])
        end_offset = int(sys.argv[3])
        duration = end_offset - start_offset
        print(f"Capturing {duration}s clip (from {start_offset}s to {end_offset}s)...")
        path = capture_clip(start_offset, end_offset)
        if path:
            print(f"Clip captured: {path}")
    
    elif command == "status":
        status = get_status()
        if status:
            import json
            print(json.dumps(status, indent=2))
        else:
            print("Failed to get status")
    
    elif command == "frames":
        print("Consuming frames from shared memory...")
        print("Press Ctrl+C to stop")
        
        with FrameSubscriber() as sub:
            if not sub._mm:
                print("Failed to connect. Is the daemon running with raw_sharing enabled?")
                sys.exit(1)
            
            print(f"Resolution: {sub.width}x{sub.height}")
            
            count = 0
            start = time.time()
            
            try:
                while True:
                    frame, meta = sub.read_frame(timeout_ms=1000)
                    if frame is not None:
                        count += 1
                        elapsed = time.time() - start
                        fps = count / elapsed if elapsed > 0 else 0
                        print(f"\rFrames: {count} | Seq: {meta['sequence']} | FPS: {fps:.1f}   ", end='', flush=True)
            except KeyboardInterrupt:
                print(f"\nTotal frames: {count}")
    
    elif command in ("-h", "--help", "help"):
        print_usage()
    
    else:
        print(f"Unknown command: {command}")
        print_usage()
        sys.exit(1)
