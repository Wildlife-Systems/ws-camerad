#pragma once

#include <cstdint>
#include <string>
#include <chrono>
#include <memory>
#include <functional>
#include <vector>
#include <atomic>

namespace camera_daemon {

// Version info
constexpr const char* VERSION = "1.0.0";

// Default paths
constexpr const char* DEFAULT_SOCKET_PATH = "/run/ws-camerad/control.sock";
constexpr const char* DEFAULT_STILLS_DIR = "/var/ws/camerad/stills";
constexpr const char* DEFAULT_CLIPS_DIR = "/var/ws/camerad/clips";
constexpr const char* DEFAULT_SHM_NAME = "/ws_camera_frames";
constexpr const char* DEFAULT_BGR_SHM_NAME = "/ws_camera_frames_bgr";

// Pixel format constants (used in FrameMetadata.format)
constexpr uint8_t PIXFMT_YUV420 = 1;
constexpr uint8_t PIXFMT_BGR24 = 4;

// Frame metadata
struct FrameMetadata {
    uint64_t timestamp_us;      // Microseconds since epoch
    uint64_t sequence;          // Frame sequence number
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t size;
    bool is_keyframe;
    uint8_t format;             // 0=NV12, 1=YUV420, 2=H264, 3=H265
    int dmabuf_fd = -1;         // DMA-BUF file descriptor for zero-copy (-1 if not available)
};

// Encoded frame with metadata
struct EncodedFrame {
    FrameMetadata metadata;
    std::vector<uint8_t> data;
};

// Camera configuration
struct CameraConfig {
    uint32_t width = 1280;
    uint32_t height = 960; 
    uint32_t framerate = 30;
    uint32_t bitrate = 4000000;
    uint32_t keyframe_interval = 30; // One keyframe per second at 30fps
    uint32_t jpeg_quality = 90;
    uint32_t rotation = 0;    // Frame rotation: 0, 90, 180, 270 degrees
    bool hflip = false;       // Horizontal flip (mirror) via ISP
    bool vflip = false;       // Vertical flip via ISP
    std::string tuning_file;  // libcamera tuning file (e.g. imx219_noir.json for NoIR modules)
};

// Virtual camera output configuration
struct VirtualCameraConfig {
    std::string device;     // e.g., "/dev/video10"
    std::string label;      // Optional display name for the virtual camera
    bool enabled = true;
    uint32_t width = 0;     // Output width (0 = use camera resolution)
    uint32_t height = 0;    // Output height (0 = use camera resolution)
};

// Daemon configuration
struct DaemonConfig {
    std::string socket_path = DEFAULT_SOCKET_PATH;
    std::string stills_dir = DEFAULT_STILLS_DIR;
    std::string clips_dir = DEFAULT_CLIPS_DIR;
    std::string shm_name = DEFAULT_SHM_NAME;
    std::string bgr_shm_name = DEFAULT_BGR_SHM_NAME;
    
    uint32_t ring_buffer_seconds = 30;  // Pre-event buffer
    uint32_t post_event_seconds = 10;   // Post-event recording
    
    bool enable_rtsp = true;
    uint16_t rtsp_port = 8554;
    
    bool enable_raw_sharing = false;    // Share raw YUV frames via shm
    bool enable_bgr_sharing = false;    // Share BGR frames via shm (for OpenCV)
    
    // Virtual camera outputs (v4l2loopback)
    // Zero or more virtual cameras can be configured
    std::vector<VirtualCameraConfig> virtual_cameras;

    // Audio recording from ws-audiod
    bool enable_audio = false;
    std::string audio_shm_name = "/ws_audiod_samples";
    uint32_t audio_buffer_seconds = 60;
    bool enable_rtsp_audio = false;  // Include audio in RTSP stream
    
    CameraConfig camera;
};

// Callback types
using FrameCallback = std::function<void(const FrameMetadata&, const uint8_t*, size_t)>;
using EncodedFrameCallback = std::function<void(const EncodedFrame&)>;

// Utility functions
inline uint64_t get_timestamp_us() {
    using namespace std::chrono;
    return duration_cast<microseconds>(
        system_clock::now().time_since_epoch()
    ).count();
}

inline std::string timestamp_to_filename() {
    auto now = std::chrono::system_clock::now();
    auto time_t = std::chrono::system_clock::to_time_t(now);
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        now.time_since_epoch()
    ).count() % 1000;
    
    char buf[64];
    std::strftime(buf, sizeof(buf), "%Y%m%d_%H%M%S", std::localtime(&time_t));
    return std::string(buf) + "_" + std::to_string(ms);
}

} // namespace camera_daemon
