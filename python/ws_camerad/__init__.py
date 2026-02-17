"""
ws_camerad - Python client library for ws-camerad camera daemon.

This module provides a Python interface for interacting with the camera daemon.

Classes:
    CameraClient: Control interface for still/clip capture
    FrameSubscriber: Shared memory frame access  
    Response: JSON response parsing

Convenience functions:
    capture_still(time_offset=0) -> Optional[str]
    capture_clip(start_offset=-5, end_offset=5) -> Optional[str]
    get_status() -> Optional[dict]

Example:
    from ws_camerad import CameraClient, capture_still, capture_clip
    
    # Quick capture
    path = capture_still()
    path = capture_clip(-5, 5)  # 10s clip
    
    # With client object
    with CameraClient() as client:
        response = client.capture_still(-2)  # 2s ago
        if response.success:
            print(response.path)
"""

import socket
import struct
import mmap
import os
import time
import json
from dataclasses import dataclass
from typing import Optional, Tuple, Any

try:
    import numpy as np
    HAS_NUMPY = True
except ImportError:
    HAS_NUMPY = False

__version__ = "1.0.0"
__all__ = [
    "CameraClient",
    "FrameSubscriber",
    "BGRFrameReader",
    "Response",
    "capture_still",
    "capture_burst",
    "capture_clip",
    "get_status",
    "read_frames",
    "DEFAULT_SOCKET_PATH",
    "DEFAULT_SHM_NAME",
    "DEFAULT_BGR_SHM_NAME",
    "DEFAULT_FRAME_NOTIFY_PATH",
    "FrameNotifySocket",
]

# Default paths
DEFAULT_SOCKET_PATH = "/run/ws-camerad/control.sock"
DEFAULT_FRAME_NOTIFY_PATH = "/run/ws-camerad/frames.sock"
DEFAULT_SHM_NAME = "/ws_camera_frames"
DEFAULT_BGR_SHM_NAME = "/ws_camera_frames_bgr"

# Header size for shared memory (matches C++ FramePublisher::Header)
HEADER_SIZE = 64


@dataclass
class Response:
    """Response from the daemon (JSON format).
    
    Response format:
        Success: {"ok": true, "path": "..."}  or  {"ok": true, "data": {...}}
        Error:   {"ok": false, "error": "..."}
    
    Attributes:
        success: True if the command succeeded
        path: File path for STILL/CLIP commands
        data: Dictionary data for STATUS commands
        error: Error message on failure
        raw: Raw response string
    """
    success: bool
    path: Optional[str] = None
    data: Optional[Any] = None
    error: Optional[str] = None
    raw: str = ""
    
    @classmethod
    def from_string(cls, s: str) -> 'Response':
        """Parse a JSON response string."""
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
        
        Returns:
            Response with .path containing the image file path on success
        """
        if time_offset == 0:
            return self.send_command("STILL")
        return self.send_command(f"STILL {time_offset}")
    
    def capture_burst(self, count: int = 5, interval_ms: int = 0) -> Response:
        """Burst capture (multiple stills in rapid succession).
        
        Args:
            count: Number of images to capture (default 5, max 100)
            interval_ms: Milliseconds between captures (0 = as fast as possible)
        
        Returns:
            Response with .data containing {"paths": [...], "count": N}
        
        Examples:
            response = client.capture_burst(10)       # 10 images, max speed
            response = client.capture_burst(5, 100)   # 5 images, 100ms apart
            paths = response.data.get("paths", [])
        """
        timeout = 5 + count  # Scale timeout with count
        return self.send_command(f"BURST {count} {interval_ms}", timeout=timeout)
    
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
        # Allow extra time for post-event recording plus processing
        timeout = abs(start_offset) + max(0, end_offset) + 30
        return self.send_command(f"CLIP {start_offset} {end_offset}", timeout=timeout)
    
    def set_parameter(self, key: str, value: str) -> Response:
        """Set a camera parameter."""
        return self.send_command(f"SET {key} {value}")
    
    def get_status(self) -> Response:
        """Get daemon status.
        
        Returns:
            Response with .data containing status dictionary on success
        """
        return self.send_command("GET STATUS")
    
    def __enter__(self):
        self.connect()
        return self
    
    def __exit__(self, *args):
        self.disconnect()


class FrameSubscriber:
    """
    Subscriber for raw video frames via shared memory.
    
    Requires numpy to be installed.
    
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
        if not HAS_NUMPY:
            print("FrameSubscriber requires numpy. Install with: pip install numpy")
            return False
            
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
    
    def read_frame(self, timeout_ms: int = -1) -> Tuple[Optional[Any], Optional[dict]]:
        """
        Read the latest frame from shared memory.
        
        Args:
            timeout_ms: Timeout in milliseconds (-1 = infinite, 0 = no wait)
            
        Returns:
            Tuple of (frame_data, metadata) or (None, None) on timeout.
            frame_data is a numpy array if numpy is available.
        """
        if not self._mm:
            return None, None
        
        if not HAS_NUMPY:
            return None, None
        
        import numpy as np
        
        # Rate-limit: sleep for remainder of min interval
        if self._min_interval > 0 and self._last_return_time > 0:
            since = time.time() - self._last_return_time
            remaining = self._min_interval - since
            if remaining > 0:
                time.sleep(remaining)
        
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
                
                self._last_return_time = time.time()
                return frame, header
            
            # Check timeout
            if timeout_ms == 0:
                return None, None
            
            if timeout_ms > 0:
                elapsed_ms = (time.time() - start_time) * 1000
                if elapsed_ms >= timeout_ms:
                    return None, None
            
            time.sleep(0.0001)  # 100us
    
    def set_min_interval(self, ms: int) -> None:
        """Set minimum interval between returned frames (max FPS cap).
        
        When set, read_frame() will sleep for the remainder of the interval
        after the previous frame was returned before polling for a new one.
        This dramatically reduces syscall count for consumers that don't need
        every camera frame.
        
        Args:
            ms: Minimum milliseconds between returned frames (0 = disabled)
        """
        self._min_interval = ms / 1000.0

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
    
    This reader is optimized for OpenCV usage - frames are returned as
    (height, width, 3) BGR numpy arrays ready for cv2 functions.
    
    Requires: numpy, enable_bgr_sharing=true in daemon config
    
    Example:
        with BGRFrameReader() as reader:
            print(f"Resolution: {reader.width}x{reader.height}")
            
            for frame, metadata in reader:
                # frame is ready for OpenCV
                cv2.imshow('Camera', frame)
                if cv2.waitKey(1) == ord('q'):
                    break
    
    Or using read_frame() directly:
        reader = BGRFrameReader()
        if reader.connect():
            while True:
                frame, meta = reader.read_frame(timeout_ms=100)
                if frame is not None:
                    process(frame)
    """
    
    def __init__(self, shm_name: str = DEFAULT_BGR_SHM_NAME):
        """
        Initialize BGR frame reader.
        
        Args:
            shm_name: Shared memory name (default: /ws_camera_frames_bgr)
        """
        self.shm_name = shm_name
        self._fd: Optional[int] = None
        self._mm: Optional[mmap.mmap] = None
        self._last_sequence: int = 0
        self._min_interval: float = 0.0  # seconds
        self._last_return_time: float = 0.0
        
        # Frame info (populated on connect)
        self.width: int = 0
        self.height: int = 0
        self.stride: int = 0
        self.frame_size: int = 0
    
    def connect(self) -> bool:
        """Connect to the shared memory.
        
        Returns:
            True on success, False on failure
        """
        if not HAS_NUMPY:
            print("BGRFrameReader requires numpy. Install with: pip install numpy")
            return False
        
        try:
            shm_path = f"/dev/shm{self.shm_name}"
            if not os.path.exists(shm_path):
                print(f"Shared memory not found: {shm_path}")
                print("Make sure the daemon is running with 'enable_bgr_sharing = true'")
                return False
            
            self._fd = os.open(shm_path, os.O_RDONLY)
            stat = os.fstat(self._fd)
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
        header_data = self._mm.read(HEADER_SIZE)
        
        # Parse header fields
        sequence = struct.unpack_from("Q", header_data, 0)[0]
        frame_ready = struct.unpack_from("I", header_data, 8)[0]
        self.width = struct.unpack_from("I", header_data, 12)[0]
        self.height = struct.unpack_from("I", header_data, 16)[0]
        self.stride = struct.unpack_from("I", header_data, 20)[0]
        self.frame_size = struct.unpack_from("I", header_data, 28)[0]
        timestamp = struct.unpack_from("Q", header_data, 32)[0]
        
        return {
            'sequence': sequence,
            'frame_ready': frame_ready,
            'width': self.width,
            'height': self.height,
            'stride': self.stride,
            'frame_size': self.frame_size,
            'timestamp_us': timestamp,
        }
    
    def read_frame(self, timeout_ms: int = 100) -> Tuple[Optional[Any], Optional[dict]]:
        """
        Read the latest BGR frame from shared memory.
        
        Args:
            timeout_ms: Timeout in milliseconds (-1 = infinite, 0 = no wait)
            
        Returns:
            Tuple of (bgr_frame, metadata) or (None, None) on timeout.
            bgr_frame is a (height, width, 3) numpy array in BGR format.
        """
        if not self._mm or not HAS_NUMPY:
            return None, None
        
        import numpy as np
        
        # Rate-limit: sleep for remainder of min interval
        if self._min_interval > 0 and self._last_return_time > 0:
            since = time.time() - self._last_return_time
            remaining = self._min_interval - since
            if remaining > 0:
                time.sleep(remaining)
        
        start_time = time.time()
        
        while True:
            header = self._read_header()
            
            if header.get('sequence', 0) > self._last_sequence and header.get('frame_ready', 0):
                self._last_sequence = header['sequence']
                
                # Read BGR frame data
                self._mm.seek(HEADER_SIZE)
                frame_bytes = self._mm.read(self.frame_size)
                
                # Reshape to BGR image (height x width x 3)
                frame = np.frombuffer(frame_bytes, dtype=np.uint8).copy()
                frame = frame.reshape((self.height, self.width, 3))
                
                self._last_return_time = time.time()
                return frame, header
            
            # Check timeout
            if timeout_ms == 0:
                return None, None
            
            if timeout_ms > 0:
                elapsed_ms = (time.time() - start_time) * 1000
                if elapsed_ms >= timeout_ms:
                    return None, None
            
            time.sleep(0.001)  # 1ms
    
    def set_min_interval(self, ms: int) -> None:
        """Set minimum interval between returned frames (max FPS cap).
        
        When set, read_frame() will sleep for the remainder of the interval
        after the previous frame was returned before polling for a new one.
        This dramatically reduces syscall count for consumers that don't need
        every camera frame.
        
        Args:
            ms: Minimum milliseconds between returned frames (0 = disabled)
        """
        self._min_interval = ms / 1000.0

    def __iter__(self):
        """Iterate over frames. Yields (frame, metadata) tuples."""
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


def capture_burst(count: int = 5, interval_ms: int = 0,
                  socket_path: str = DEFAULT_SOCKET_PATH) -> list:
    """Burst capture and return list of file paths.
    
    Args:
        count: Number of images to capture (default 5, max 100)
        interval_ms: Milliseconds between captures (0 = as fast as possible)
        socket_path: Path to daemon socket
    
    Returns:
        List of file paths on success, empty list on failure
    """
    with CameraClient(socket_path) as client:
        response = client.capture_burst(count, interval_ms)
        if response.success and response.data:
            return response.data.get("paths", [])
        else:
            print(f"Error: {response.error}")
            return []


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
    """Get daemon status as a dictionary.
    
    Returns:
        Status dictionary on success, None on failure
    """
    with CameraClient(socket_path) as client:
        response = client.get_status()
        if response.success:
            return response.data if response.data else {'raw': response.raw}
        return None


def read_frames(shm_name: str = DEFAULT_BGR_SHM_NAME, timeout_ms: int = 100):
    """
    Generator that yields BGR frames from shared memory.
    
    This is the simplest way to read frames for OpenCV processing.
    Requires: numpy, enable_bgr_sharing=true in daemon config
    
    Args:
        shm_name: Shared memory name (default: BGR frames)
        timeout_ms: Timeout per frame read (ms)
    
    Yields:
        Tuple of (bgr_frame, metadata) where bgr_frame is a (H, W, 3) numpy array
    
    Example:
        from ws_camerad import read_frames
        import cv2
        
        for frame, meta in read_frames():
            cv2.imshow('Camera', frame)
            if cv2.waitKey(1) == ord('q'):
                break
        cv2.destroyAllWindows()
    """
    with BGRFrameReader(shm_name) as reader:
        if reader.width == 0:
            return  # Failed to connect
        
        for frame, meta in reader:
            yield frame, meta


class FrameNotifySocket:
    """Subscribe to frame notifications from the daemon.

    Instead of polling shared memory, connect to the daemon's notification
    socket and block until a new frame is published. Each notification is
    an 8-byte uint64 sequence number.

    Usage::

        from ws_camerad import FrameNotifySocket, BGRFrameReader

        notify = FrameNotifySocket()
        notify.connect()

        reader = BGRFrameReader()
        reader.connect()

        while True:
            seq = notify.wait()           # blocks until next frame
            frame, meta = reader.read_frame(timeout_ms=0)  # immediate
            if frame is not None:
                process(frame)

    This replaces ~45,000 polling syscalls per 300 frames with 300 blocking
    reads — one per frame.
    """

    def __init__(self, socket_path: str = DEFAULT_FRAME_NOTIFY_PATH):
        self.socket_path = socket_path
        self._sock: Optional[socket.socket] = None

    def connect(self) -> bool:
        """Connect to the daemon's frame notification socket.

        Returns:
            True on success, False on failure.
        """
        if self._sock is not None:
            return True
        try:
            self._sock = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
            self._sock.connect(self.socket_path)
            return True
        except OSError:
            self._sock = None
            return False

    def disconnect(self) -> None:
        """Disconnect from the notification socket."""
        if self._sock is not None:
            try:
                self._sock.close()
            except OSError:
                pass
            self._sock = None

    @property
    def is_connected(self) -> bool:
        return self._sock is not None

    def wait(self, timeout: Optional[float] = None) -> int:
        """Block until the next frame is published.

        Args:
            timeout: Timeout in seconds (None = block forever).

        Returns:
            The sequence number of the new frame, or 0 on timeout/error.
        """
        if self._sock is None:
            return 0
        try:
            if timeout is not None:
                self._sock.settimeout(timeout)
            else:
                self._sock.settimeout(None)
            data = self._sock.recv(8)
            if len(data) != 8:
                self.disconnect()
                return 0
            return struct.unpack("Q", data)[0]
        except socket.timeout:
            return 0
        except OSError:
            self.disconnect()
            return 0

    def __enter__(self):
        self.connect()
        return self

    def __exit__(self, *args):
        self.disconnect()

    def fileno(self) -> int:
        """Return the underlying fd for use with select/poll/epoll."""
        if self._sock is None:
            raise ValueError("Not connected")
        return self._sock.fileno()
