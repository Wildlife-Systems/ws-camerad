#pragma once

#include "common.hpp"
#include "camera_manager.hpp"
#include "v4l2_encoder.hpp"
#include "ring_buffer.hpp"
#include "shared_memory.hpp"
#include "frame_notifier.hpp"
#include "still_capture.hpp"
#include "clip_extractor.hpp"
#include "rtsp_server.hpp"
#include "frame_rotator.hpp"
#include "v4l2_loopback.hpp"
#include <memory>

namespace camera_daemon {

/**
 * Main capture pipeline that orchestrates all components.
 * This is the central coordinator for the camera daemon.
 */
class CapturePipeline {
public:
    explicit CapturePipeline(const DaemonConfig& config);
    ~CapturePipeline();

    // Non-copyable
    CapturePipeline(const CapturePipeline&) = delete;
    CapturePipeline& operator=(const CapturePipeline&) = delete;

    /**
     * Initialize all pipeline components.
     */
    bool initialize();

    /**
     * Start the capture pipeline.
     */
    bool start();

    /**
     * Stop the capture pipeline.
     */
    void stop();

    /**
     * Check if pipeline is running.
     */
    bool is_running() const { return running_.load(); }

    /**
     * Request a still capture.
     * @param time_offset_seconds negative=past, positive=future, 0=now
     * @return Path to the captured image, or empty on error
     */
    std::string capture_still(int time_offset_seconds = 0);

    /**
     * Request a burst capture (multiple stills in rapid succession).
     * @param count Number of images to capture
     * @param interval_ms Milliseconds between captures (0 = as fast as possible)
     * @return Vector of paths to captured images
     */
    std::vector<std::string> capture_burst(int count, int interval_ms = 0);

    /**
     * Request a video clip.
     * @param start_offset Start time relative to now (negative=past, positive=future)
     * @param end_offset End time relative to now (negative=past, positive=future)
     * @return Path to the clip, or empty on error
     */
    std::string capture_clip(int start_offset, int end_offset);

    /**
     * Set camera parameter.
     */
    bool set_parameter(const std::string& key, const std::string& value);

    /**
     * Warm-restart camera with a new tuning file.
     * Stops only camera + encoder, keeps RTSP/shm/loopback alive.
     * Clients see a brief frame gap (~0.5-1s) then seamless recovery.
     * @param new_tuning_file Tuning filename or full path
     * @return true on success
     */
    bool restart_camera(const std::string& new_tuning_file);

    /**
     * Get current status as JSON.
     */
    std::string get_status_json() const;

    /**
     * Get statistics.
     */
    struct Stats {
        uint64_t frames_captured;
        uint64_t frames_encoded;
        uint64_t frames_dropped;
        double capture_fps;
        double encode_fps;
        size_t ring_buffer_frames;
        size_t ring_buffer_bytes;
    };
    Stats get_stats() const;

private:
    void on_raw_frame(const FrameMetadata& metadata, const uint8_t* data, size_t size);
    void on_encoded_frame(const EncodedFrame& frame);

    DaemonConfig config_;
    std::atomic<bool> running_{false};

    // Pipeline components
    std::unique_ptr<CameraManager> camera_manager_;
    std::unique_ptr<V4L2Encoder> encoder_;
    bool encoder_initialized_ = false;
    V4L2Encoder::Config pending_enc_config_;
    std::unique_ptr<EncodedRingBuffer> ring_buffer_;
    std::unique_ptr<FramePublisher> frame_publisher_;      // Raw YUV frames
    std::unique_ptr<FramePublisher> bgr_frame_publisher_;  // BGR frames for OpenCV
    std::unique_ptr<FrameNotifier> frame_notifier_;        // Notification socket
    std::unique_ptr<StillCapture> still_capture_;
    std::unique_ptr<ClipExtractor> clip_extractor_;
    std::unique_ptr<RTSPServer> rtsp_server_;

    // Frame rotation (90°/270° only; 0°/180° handled by ISP)
    std::unique_ptr<FrameRotator> frame_rotator_;

    // Virtual camera outputs (v4l2loopback)
    std::vector<std::unique_ptr<V4L2LoopbackOutput>> virtual_cameras_;

    // BGR conversion buffer
    std::vector<uint8_t> bgr_buffer_;

    // YUV420 conversion buffer (for YUYV→YUV420 or MJPEG→YUV420)
    std::vector<uint8_t> yuv420_buffer_;

    // Pixel format the camera actually delivers (may differ from YUV420)
    uint8_t camera_pixel_format_ = PIXFMT_YUV420;

    // Statistics
    std::atomic<uint64_t> frames_captured_{0};
    std::atomic<uint64_t> frames_encoded_{0};
    std::atomic<uint64_t> frames_dropped_{0};
    
    uint64_t stats_start_time_ = 0;
    uint64_t last_stats_time_ = 0;
    uint64_t last_capture_count_ = 0;
    uint64_t last_encode_count_ = 0;
    double capture_fps_ = 0.0;
    double encode_fps_ = 0.0;
};

} // namespace camera_daemon
