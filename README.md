# ws-camerad

[![Build](https://github.com/Wildlife-Systems/ws-camerad/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/Wildlife-Systems/ws-camerad/actions/workflows/build.yml)

A multi-consumer camera daemon for Raspberry Pi sensor networks.

In distributed camera-based monitoring systems, the principal challenge is not frame acquisition but rather the concurrent distribution of captured frames to multiple consumers. Motion detectors, machine learning classifiers, human operators, and archival systems each require access to the same frame data, potentially at different times and in different formats.

ws-camerad provides continuous capture with concurrent multi-consumer access through a single camera interface. Still images and video clips may be extracted from any point in the rolling buffer, including frames that have already elapsed.

## Motivation

On Linux-based embedded systems, a single process holds exclusive access to a camera device at any given time. This constraint is the fundamental limitation that ws-camerad addresses.

ws-camerad operates as infrastructure rather than an end-user application. The daemon maintains exclusive camera ownership; consumers connect via shared memory (zero-copy frame access), Unix domain socket (control interface), or RTSP (remote access). Consumer process failures are isolated and do not affect the daemon or other connected consumers.

ws-camerad is intended for custom deployments that require reliable, shared camera infrastructure without reimplementing low-level capture and distribution logic.

## Features

- Continuous capture with zero frame loss
- Concurrent multi-consumer access (e.g. with provided C++ and Python client libraries)
- On-demand still image capture
- Pre-event and post-event video clip extraction from rolling buffer
- Remote streaming via RTSP
- Hardware-accelerated H.264 encoding via V4L2 M2M
- Zero-copy frame distribution via POSIX shared memory
- Software frame rotation (0°/90°/180°/270°) using ARM NEON SIMD
- Burst capture mode for rapid multi-frame acquisition
- Virtual camera output via v4l2loopback kernel module

## Architecture

For detailed technical descriptions of the daemon's internals and usage, see [Architecture of ws-camerad](https://ebaker.me.uk/2026/02/22/architecture-of-ws-camerad.html)

## Usage guides

- [Rotating a Pi Camera Feed 90° with ws-camerad](https://ebaker.me.uk/2026/02/21/rotating-pi-camera-feed-with-ws-camerad.html) — configuring software rotation via v4l2loopback
- [Switching Pi NoIR Camera Profiles at Runtime — Without Restarting Anything](https://ebaker.me.uk/2026/02/17/switching-pi-camera-colour-profiles.html) — runtime tuning file switching

## Building from Source

### Prerequisites

The following packages are required on Raspberry Pi OS:

```bash
sudo apt update
sudo apt install -y cmake build-essential libcamera-dev libjpeg-dev pkg-config
```

### Compilation

```bash
git clone https://github.com/Wildlife-Systems/ws-camerad.git
cd ws-camerad
mkdir build && cd build
cmake ..
make -j4
```

### Installation

```bash
sudo make install
sudo mkdir -p /var/ws/camerad/{stills,clips}
sudo cp config/camera-daemon.service /etc/systemd/system/
sudo systemctl daemon-reload
```

## Usage

```bash
# Run directly
./ws-camerad

# With options
./ws-camerad -W 1920 -H 1080 -f 30 -d

# As a service
sudo systemctl start ws-camerad
```

### Options

| Option | Description |
|--------|-------------|
| `-c, --config FILE` | Configuration file path |
| `-s, --socket PATH` | Control socket path |
| `-W, --width` | Video width (default: 1280) |
| `-H, --height` | Video height (default: 960) |
| `-f, --framerate` | Frame rate (default: 30) |
| `-b, --bitrate` | Bitrate (default: 4000000) |
| `-t, --tuning-file` | Tuning file for NoIR modules |
| `-o, --rotation` | Frame rotation (0, 90, 180, 270) |
| `-r, --rtsp-port` | RTSP server port (default: 8554) |
| `-R, --no-rtsp` | Disable RTSP streaming |
| `-d, --debug` | Enable debug logging |

### Commands

The daemon accepts commands via the Unix domain control socket:

```bash
python3 examples/still_client.py
./clip_client -5 5
```

| Command | Description |
|---------|-------------|
| `STILL [offset]` | Capture a JPEG still (offset: 0 = current frame, negative = past frame) |
| `BURST <count> [interval_ms]` | Capture multiple consecutive stills |
| `CLIP <start> <end>` | Extract a video clip (offsets in seconds relative to current time) |
| `SET <key> <value>` | Modify a camera parameter at runtime (see below) |
| `GET STATUS` | Retrieve current daemon status |

**SET parameters:**
- Camera controls (applied immediately, per-frame): `exposure`, `gain`, `brightness`, `contrast`, `sharpness`, `saturation`, `ae_enable`, `awb_enable`, `exposure_value`
- Tuning file (triggers warm restart, approximately 0.5–1s frame gap): `SET tuning_file imx219_noir.json`

**Clip offset examples:**
- `CLIP -5 5` — 10-second clip spanning 5 seconds before and 5 seconds after the current time
- `CLIP -10 0` — 10-second clip drawn entirely from the buffer

All responses are returned in JSON format:
```json
{"ok":true,"path":"/var/ws/camerad/stills/still_20260209_173407_11.jpg"}
{"ok":true,"data":{"running":true,"capture":{"frames":826,"fps":29.97}}}
{"ok":false,"error":"Invalid command"}
```

### Client Examples

**Python client library:**
```python
from ws_camerad import CameraClient

with CameraClient() as client:
    response = client.capture_still()
    print(response.path)
    
    response = client.capture_clip(-5, 5)
    print(response.path)
```

**Shared memory consumer (zero-copy):**
```bash
./frame_consumer
python3 examples/camera_client.py frames
```

**TCP stream (remote access):**
```bash
ffplay tcp://raspberry-pi:8554
ffmpeg -i tcp://raspberry-pi:8554 -c copy output.mp4
```

## Configuration

`/etc/ws/camerad/ws-camerad.conf`:

```ini
[daemon]
socket_path = /run/ws-camerad/control.sock
stills_dir = /var/ws/camerad/stills
clips_dir = /var/ws/camerad/clips
ring_buffer_seconds = 30
enable_rtsp = true
rtsp_port = 8554

[camera]
width = 1280
height = 960
framerate = 30
bitrate = 4000000
jpeg_quality = 90
# rotation = 0
# tuning_file = imx219_noir.json
```

## Virtual Camera Output

ws-camerad writes frames to v4l2loopback devices, enabling any V4L2-compatible application (OpenCV, FFmpeg, OBS, web browsers) to consume the feed as a standard video device.

```bash
# Install the v4l2loopback kernel module
sudo apt install v4l2loopback-dkms v4l2loopback-utils

# Load module
sudo modprobe v4l2loopback devices=2 video_nr=10,11 card_label="Virtual Camera 1,Virtual Camera 2"
```

Configuration:

```ini
[camera]
rotation = 90

[virtual_camera.0]
device = /dev/video10
enabled = true

[virtual_camera.1]
device = /dev/video11
width = 640
height = 480
enabled = true
```

Virtual cameras may output at a lower resolution than the source capture. Specifying `width` and `height` per virtual camera instance enables automatic YUV420 downsampling. Omitting these values or setting them to 0 outputs frames at the full camera resolution.


A maximum of 8 virtual camera devices is supported.

To configure persistent module loading across reboots:

```bash
echo "v4l2loopback" | sudo tee /etc/modules-load.d/v4l2loopback.conf
echo "options v4l2loopback devices=4 video_nr=10,11,12,13" | sudo tee /etc/modprobe.d/v4l2loopback.conf
```

## Multi-Camera Deployment

Multiple daemon instances may be run concurrently with distinct configurations. Each camera requires a unique control socket path, shared memory identifier, and RTSP port.

```ini
# /etc/ws/camerad/front_door.conf
[daemon]
socket_path = /run/ws-camerad/front_door.sock
stills_dir = /var/ws/camerad/front_door/stills
clips_dir = /var/ws/camerad/front_door/clips
shm_name = /ws_camerad_frames_front_door
rtsp_port = 8554

[camera]
camera_id = 0
```

A systemd template unit simplifies management of multiple instances:

```bash
sudo cat > /etc/systemd/system/ws-camerad@.service << 'EOF'
[Unit]
Description=ws-camerad (%i)
After=network.target

[Service]
Type=simple
ExecStart=/usr/bin/ws-camerad -c /etc/ws/camerad/%i.conf
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
EOF

sudo systemctl daemon-reload
sudo systemctl enable --now ws-camerad@front_door
sudo systemctl enable --now ws-camerad@backyard
```

Client usage with multiple cameras:

```python
from ws_camerad import CameraClient
from concurrent.futures import ThreadPoolExecutor

cameras = {
    "front_door": CameraClient("/run/ws-camerad/front_door.sock"),
    "backyard": CameraClient("/run/ws-camerad/backyard.sock"),
}

# Capture from all cameras in parallel
with ThreadPoolExecutor() as pool:
    futures = {name: pool.submit(cam.capture_still) for name, cam in cameras.items()}
    results = {name: f.result() for name, f in futures.items()}
```

Resource consumption per instance: approximately 4–8% CPU at 1080p30, 30–50 MB memory. Practical limits on the Raspberry Pi 4: 2–3 cameras at 1080p30, or 4–6 cameras at 720p30.

To enumerate available cameras: `libcamera-hello --list-cameras`

## Frame Rotation

```bash
ws-camerad --rotation 90

# Or in config
[camera]
rotation = 90
```

| Rotation | Method | Cost |
|----------|--------|------|
| 0° | Identity | Free |
| 180° | ISP hardware flip | Free |
| 90°/270° | NEON SIMD 8×8 transpose | ~7ms |

The Raspberry Pi ISP natively supports only horizontal and vertical flip operations. For 90° and 270° rotations, the daemon performs software rotation of all three YUV420 planes using an ARM NEON 8×8 block transpose algorithm with parallel processing of the Y, U, and V planes.

All downstream consumers receive pre-rotated frames without additional processing.

### NoIR Camera Tuning

NoIR (No Infrared filter) camera modules exhibit a pink color cast under standard auto white balance settings. Dedicated tuning files correct this behavior:

```bash
ws-camerad --tuning-file imx219_noir.json
```

Available tuning files (resolved to `/usr/share/libcamera/ipa/rpi/vc4/`):
- `imx219_noir.json` — Camera Module v2 NoIR
- `imx477_noir.json` — HQ Camera NoIR
- `imx708_noir.json` — Camera Module v3 NoIR

#### Runtime Tuning File Switching

The tuning file may be changed while the daemon is running. This operation triggers a warm restart of the camera and encoder (approximately 0.5–1 second frame gap) while preserving active RTSP streams, shared memory mappings, and virtual camera devices. Connected clients experience a brief interruption followed by seamless recovery.

```bash
# Switch to NoIR profile at sunset
echo "SET tuning_file imx219_noir.json" | socat - UNIX-CONNECT:/run/ws-camerad/control.sock

# Switch back to standard profile at sunrise
echo "SET tuning_file imx219.json" | socat - UNIX-CONNECT:/run/ws-camerad/control.sock
```

## License

GPL-2.0-or-later
