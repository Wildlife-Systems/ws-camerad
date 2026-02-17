#pragma once

#include "common.hpp"
#include <string>
#include <vector>
#include <atomic>
#include <cstdint>

namespace camera_daemon {

/**
 * Writes frames to a v4l2loopback virtual camera device.
 * 
 * Requires the v4l2loopback kernel module to be loaded:
 *   sudo modprobe v4l2loopback devices=1 video_nr=10 card_label="Virtual Camera"
 * 
 * The virtual camera will appear as a standard V4L2 capture device that
 * can be opened by any application (OpenCV, ffmpeg, browsers, etc.).
 */
class V4L2LoopbackOutput {
public:
    struct Config {
        std::string device;         // e.g., "/dev/video10"
        uint32_t width = 1280;
        uint32_t height = 960;
        uint32_t framerate = 30;
        std::string label;          // Optional display name
    };

    V4L2LoopbackOutput();
    ~V4L2LoopbackOutput();

    // Non-copyable
    V4L2LoopbackOutput(const V4L2LoopbackOutput&) = delete;
    V4L2LoopbackOutput& operator=(const V4L2LoopbackOutput&) = delete;

    /**
     * Initialize the loopback output device.
     * @return true on success, false if device doesn't exist or isn't a loopback
     */
    bool initialize(const Config& config);

    /**
     * Write a YUV420 frame to the virtual camera.
     * If the configured output dimensions differ from the input,
     * the frame is downsampled automatically.
     * @param yuv_data YUV420 planar data
     * @param size Size in bytes
     * @param metadata Frame metadata
     * @return true on success
     */
    bool write_frame(const uint8_t* yuv_data, size_t size, const FrameMetadata& metadata);

    /**
     * Set up downsampling from source to configured output dimensions.
     * Called automatically on first frame if not called explicitly.
     */
    void setup_downsample(uint32_t source_width, uint32_t source_height);

    /**
     * Close the device.
     */
    void close();

    /**
     * Check if the device is open and ready.
     */
    bool is_open() const { return fd_ >= 0; }

    /**
     * Get the device path.
     */
    const std::string& device() const { return config_.device; }

    /**
     * Get statistics.
     */
    struct Stats {
        uint64_t frames_written;
        uint64_t frames_dropped;
        uint64_t bytes_written;
    };
    Stats get_stats() const;

    /**
     * Check if v4l2loopback module is available on the system.
     */
    static bool is_available();

    /**
     * Find available loopback devices.
     * @return Vector of device paths (e.g., {"/dev/video10", "/dev/video11"})
     */
    static std::vector<std::string> find_loopback_devices();

private:
    bool open_device();
    bool set_format();
    void downsample_yuv420(const uint8_t* src, uint32_t src_w, uint32_t src_h,
                           uint32_t src_stride, uint8_t* dst,
                           uint32_t dst_w, uint32_t dst_h);

    Config config_;
    int fd_ = -1;

    // Downsampling state
    uint32_t source_width_ = 0;     // Expected input frame width
    uint32_t source_height_ = 0;    // Expected input frame height
    bool needs_downsample_ = false;
    std::vector<uint8_t> downsample_buf_;

    // Statistics
    std::atomic<uint64_t> frames_written_{0};
    std::atomic<uint64_t> frames_dropped_{0};
    std::atomic<uint64_t> bytes_written_{0};
};

} // namespace camera_daemon
