# ws-camerad

A camera daemon for Raspberry Pi sensor networks.

When deploying camera-based monitoring at scale, the challenge is not capturing frames—it's making them available to multiple consumers without dropping any. A motion detector, a machine learning classifier, a human operator, and a long-term archive all want the same frames, but at different times and in different formats.

ws-camerad captures continuously and serves many consumers from a single camera. Stills and video clips can be extracted from any point in the rolling buffer, including frames that have already passed.

## Why ws-camerad

Only one process can own the camera. This is the fundamental problem.

| Tool | Multi-process | Pre-event video | Low-latency stills | Notes |
|------|---------------|-----------------|-------------------|-------|
| rpicam-apps | No | No | No | Single output only |
| motion | Web clients only | Yes | No | End-user app, high CPU |
| GStreamer | Via tee | DIY | DIY | Requires deep expertise |
| PiCamera2 | Same process only | DIY | Yes | Python GIL limits parallelism |
| FFmpeg | Single output | No | No | Encoder, not a daemon |

ws-camerad is infrastructure, not an application. The daemon owns the camera; consumers connect via shared memory (zero-copy frames), Unix socket (control), or TCP (remote). If a consumer crashes, the daemon continues. Other consumers are unaffected.

Use rpicam-apps for simple capture. Use motion for a complete surveillance system. Use ws-camerad when building something custom that needs reliable camera infrastructure without reinventing it.

## Features

- Continuous capture without frame drops
- Multiple concurrent consumers (C++ and Python)
- On-demand still capture (<25ms latency)
- Pre/post-event video clips from rolling buffer
- Remote streaming (TCP)
- Hardware H.264 encoding via V4L2
- Zero-copy frame sharing via shared memory
- Frame rotation (0°/90°/180°/270°) with NEON SIMD
- Burst capture for rapid multi-still sequences
- Virtual camera output via v4l2loopback

## Architecture

```
┌────────────────────────────┐
│         ws-camerad         │
│    (C++, libcamera core)   │
└────────────┬───────────────┘
             │
 ┌───────────┼─────────────┬───────────────┐
 │           │             │               │
 │     Shared Memory   UNIX Socket     Network Stream
 │   (frames / H.264)   (control)        (TCP)
 │           │             │               │
┌▼────────┐ ┌▼────────┐ ┌──▼────────┐ ┌───▼─────────┐
│ C++ CV  │ │ Python  │ │ CLI tools │ │ Remote      │
│ consumer│ │ consumer│ │ / scripts │ │ server / NVR│
└─────────┘ └─────────┘ └───────────┘ └─────────────┘
```

## Building

Dependencies (Raspberry Pi OS):

```bash
sudo apt update
sudo apt install -y cmake build-essential libcamera-dev libjpeg-dev pkg-config
```

Build:

```bash
mkdir build && cd build
cmake ..
make -j4
```

Install:

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

Connect to the control socket:

```bash
python3 examples/still_client.py
./clip_client -5 5
```

| Command | Description |
|---------|-------------|
| `STILL [offset]` | Capture JPEG (offset: 0=now, negative=past) |
| `BURST <count> [interval_ms]` | Capture multiple stills |
| `CLIP <start> <end>` | Extract video clip (offsets relative to now) |
| `SET <key> <value>` | Set camera parameter (see below) |
| `GET STATUS` | Get daemon status |

SET parameters:
- Camera controls (instant, per-frame): `exposure`, `gain`, `brightness`, `contrast`, `sharpness`, `saturation`, `ae_enable`, `awb_enable`, `exposure_value`
- Tuning file (warm restart, ~0.5-1s gap): `SET tuning_file imx219_noir.json`

Clip examples:
- `CLIP -5 5` — 10 seconds: 5s before to 5s after now
- `CLIP -10 0` — 10 seconds: all from buffer

Responses are JSON:
```json
{"ok":true,"path":"/var/ws/camerad/stills/still_20260209_173407_11.jpg"}
{"ok":true,"data":{"running":true,"capture":{"frames":826,"fps":29.97}}}
{"ok":false,"error":"Invalid command"}
```

### Client Examples

**Python:**
```python
from ws_camerad import CameraClient

with CameraClient() as client:
    response = client.capture_still()
    print(response.path)
    
    response = client.capture_clip(-5, 5)
    print(response.path)
```

**Shared memory consumer:**
```bash
./frame_consumer
python3 examples/camera_client.py frames
```

**TCP stream:**
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

## Virtual Cameras

ws-camerad outputs frames to v4l2loopback devices. Any V4L2-compatible application (OpenCV, FFmpeg, OBS, browsers) can consume the feed as a standard video device.

```bash
# Install v4l2loopback
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

Virtual cameras can output at a lower resolution than the source. Set `width` and `height` per virtual camera to downsample the YUV420 frames automatically. Omit or set to 0 to output at full camera resolution.

Performance (Pi 4, 1280×960 @ 30fps, 90° rotation):

| Virtual Cameras | Processing Time | Headroom |
|-----------------|-----------------|----------|
| 1 | ~11ms | 22ms |
| 5 | ~14ms | 19ms |
| 8 | ~17ms | 17ms |

The module supports up to 8 devices. Each uses ~1.8MB at 1280×960.

Persistent loading:

```bash
echo "v4l2loopback" | sudo tee /etc/modules-load.d/v4l2loopback.conf
echo "options v4l2loopback devices=4 video_nr=10,11,12,13" | sudo tee /etc/modprobe.d/v4l2loopback.conf
```

## Multi-Camera Setup

Run separate daemon instances with distinct configurations. Each camera needs its own socket, shared memory name, and RTSP port.

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

systemd template unit:

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

Client usage:

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

Resources per instance: ~4-8% CPU at 1080p30, 30-50 MB memory. Practical limits on Pi 4: 2-3 cameras at 1080p30, 4-6 at 720p30.

List cameras: `libcamera-hello --list-cameras`

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

The Pi's ISP only supports horizontal and vertical flip. For 90°/270°, the daemon rotates all three YUV420 planes in software using ARM NEON 8×8 block transpose with parallel processing of Y, U, and V planes.

Performance at 1280×960 @ 30fps on Cortex-A72:

| Metric | rotation=0 | rotation=90 |
|--------|-----------|-------------|
| CPU per frame | ~0ms (DMABUF zero-copy) | ~7ms (NEON rotate) |
| Memory | No extra buffer | +1.8MB |
| Encoder input | DMABUF | USERPTR |
| Output dimensions | 1280×960 | 960×1280 |

All downstream consumers receive rotated frames automatically.

### NoIR Tuning

For NoIR camera modules (pink tint with standard AWB):

```bash
ws-camerad --tuning-file imx219_noir.json
```

Available tuning files (resolved to `/usr/share/libcamera/ipa/rpi/vc4/`):
- `imx219_noir.json` — Camera Module v2 NoIR
- `imx477_noir.json` — HQ Camera NoIR
- `imx708_noir.json` — Camera Module v3 NoIR

#### Runtime Tuning File Switch

The tuning file can be changed while the daemon is running. This performs a warm restart of the camera and encoder (~0.5-1s frame gap) while keeping RTSP streams, shared memory, and virtual cameras alive. Clients see a brief stall then seamless recovery.

```bash
# Switch to NoIR profile at sunset
echo "SET tuning_file imx219_noir.json" | socat - UNIX-CONNECT:/run/ws-camerad/control.sock

# Switch back to standard profile at sunrise
echo "SET tuning_file imx219.json" | socat - UNIX-CONNECT:/run/ws-camerad/control.sock
```

Automate with cron:
```bash
# crontab -e
30 6  * * * echo "SET tuning_file imx219.json" | socat - UNIX-CONNECT:/run/ws-camerad/control.sock
30 18 * * * echo "SET tuning_file imx219_noir.json" | socat - UNIX-CONNECT:/run/ws-camerad/control.sock
```

## File Locations

| Path | Purpose |
|------|---------|
| `/run/ws-camerad/control.sock` | Control socket |
| `/var/ws/camerad/stills/` | JPEG stills |
| `/var/ws/camerad/clips/` | Video clips |
| `/etc/ws/camerad/` | Configuration |
| `/camera_frames` | Shared memory |

## Performance

| Metric | Target | Typical |
|--------|--------|---------|
| Frame drops | 0 | 0 |
| Capture latency | <40ms | ~33ms |
| Still capture | <25ms | ~15ms |
| CPU (720p30) | <15% | ~10% |
| Memory | Bounded | ~50MB |

## License

GPL-2.0-or-later
