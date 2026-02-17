#pragma once

#include "common.hpp"
#include <libcamera/libcamera.h>
#include <memory>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>

namespace camera_daemon {

/**
 * Manages libcamera initialization, configuration, and capture.
 * This is the sole owner of the camera hardware.
 */
class CameraManager {
public:
    CameraManager();
    ~CameraManager();

    // Non-copyable
    CameraManager(const CameraManager&) = delete;
    CameraManager& operator=(const CameraManager&) = delete;

    /**
     * Initialize the camera system.
     * @return true on success
     */
    bool initialize();

    /**
     * Initialize with a specific tuning file.
     * Use e.g. "imx219_noir.json" for NoIR camera modules.
     * @param tuning_file Tuning filename or full path (empty = auto-detect)
     * @return true on success
     */
    bool initialize(const std::string& tuning_file);

    /**
     * Configure the camera with specified settings.
     */
    bool configure(const CameraConfig& config);

    /**
     * Start continuous capture.
     */
    bool start();

    /**
     * Stop capture.
     */
    void stop();

    /**
     * Check if camera is currently capturing.
     */
    bool is_running() const { return running_.load(); }

    /**
     * Register callback for raw frames.
     */
    void set_frame_callback(FrameCallback callback);

    /**
     * Get current camera configuration (updated to reflect actual negotiated values).
     */
    const CameraConfig& config() const { return config_; }

    /**
     * Get the actual pixel format negotiated with the camera.
     * May differ from requested YUV420 (e.g. YUYV or MJPEG for USB cameras).
     */
    uint8_t actual_pixel_format() const { return actual_pixel_format_; }

    /**
     * Set camera control (exposure, gain, etc.)
     */
    bool set_control(const std::string& name, int64_t value);
    bool set_control(const std::string& name, float value);
    bool set_control(const std::string& name, bool value);

private:
    void request_complete(libcamera::Request* request);
    void process_frame(libcamera::FrameBuffer* buffer, const libcamera::ControlList& metadata);

    std::unique_ptr<libcamera::CameraManager> cm_;
    std::shared_ptr<libcamera::Camera> camera_;
    std::unique_ptr<libcamera::CameraConfiguration> camera_config_;
    std::unique_ptr<libcamera::FrameBufferAllocator> allocator_;
    std::vector<std::unique_ptr<libcamera::Request>> requests_;
    libcamera::Stream* stream_ = nullptr;

    CameraConfig config_;
    uint8_t actual_pixel_format_ = PIXFMT_YUV420;
    FrameCallback frame_callback_;

    std::atomic<bool> running_{false};
    std::atomic<uint64_t> frame_sequence_{0};

    mutable std::mutex mutex_;
    std::queue<libcamera::Request*> completed_requests_;
    libcamera::ControlList pending_controls_;  // Controls to apply on next request
    
    // Stored ScalerCrop for full sensor FOV
    std::optional<libcamera::Rectangle> scaler_crop_;
};

} // namespace camera_daemon
