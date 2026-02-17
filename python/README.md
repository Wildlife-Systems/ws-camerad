# ws-camerad Python Client

Python client library for the ws-camerad camera daemon.

## Installation

```bash
# From Debian package
sudo apt install python3-ws-camerad

# From pip (if published)
pip install ws-camerad

# With frame support (requires numpy)
pip install ws-camerad[frames]
```

## Quick Start

```python
from ws_camerad import capture_still, capture_clip, get_status

# Capture a still image
path = capture_still()
print(f"Image saved to: {path}")

# Capture from 2 seconds ago
path = capture_still(-2)

# Capture a 10-second clip (5s before to 5s after now)
path = capture_clip(-5, 5)

# Capture entirely from buffer (10s ago to now)
path = capture_clip(-10, 0)

# Get daemon status
status = get_status()
print(status)
```

## CameraClient Class

For more control, use the `CameraClient` class directly:

```python
from ws_camerad import CameraClient

with CameraClient() as client:
    # Capture still
    response = client.capture_still()
    if response.success:
        print(f"Saved to: {response.path}")
    else:
        print(f"Error: {response.error}")
    
    # Capture clip
    response = client.capture_clip(-5, 5)
    if response.success:
        print(f"Clip: {response.path}")
    
    # Get status  
    response = client.get_status()
    if response.success:
        print(response.data)
```

## Frame Subscriber

Access raw video frames via shared memory (requires numpy):

```python
from ws_camerad import FrameSubscriber

with FrameSubscriber() as sub:
    if sub.width > 0:
        print(f"Resolution: {sub.width}x{sub.height}")
        
        while True:
            frame, metadata = sub.read_frame(timeout_ms=1000)
            if frame is not None:
                print(f"Frame {metadata['sequence']}")
                # Process with OpenCV, etc.
```

## Time Offset Semantics

Both STILL and CLIP commands use time offsets relative to "now":

- **Negative values** = past (from ring buffer)
- **Zero** = now
- **Positive values** = future (wait and record)

Examples:
- `capture_still(-2)` - Image from 2 seconds ago
- `capture_clip(-5, 5)` - 10s clip: 5s before to 5s after now
- `capture_clip(-10, 0)` - 10s clip entirely from buffer

## API Reference

### Functions

- `capture_still(time_offset=0, socket_path=DEFAULT_SOCKET_PATH) -> Optional[str]`
- `capture_clip(start_offset=-5, end_offset=5, socket_path=DEFAULT_SOCKET_PATH) -> Optional[str]`
- `get_status(socket_path=DEFAULT_SOCKET_PATH) -> Optional[dict]`

### Classes

- `CameraClient(socket_path, timeout)` - Control socket client
- `FrameSubscriber(shm_name)` - Shared memory frame reader
- `Response` - Parsed daemon response with `success`, `path`, `data`, `error` fields

## License

MIT License - see LICENSE file.
