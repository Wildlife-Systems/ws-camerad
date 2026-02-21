#pragma once

#include "common.hpp"
#include "raw_ring_buffer.hpp"
#include "v4l2_jpeg.hpp"
#include <string>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <thread>
#include <atomic>
#include <memory>

namespace camera_daemon {

/**
 * Handles still image capture without interrupting video stream.
 * Encodes raw frames to JPEG asynchronously.
 */
class StillCapture {
public:
    struct Config {
        std::string output_dir = DEFAULT_STILLS_DIR;
        int jpeg_quality = 90;
        bool embed_timestamp = true;
        uint32_t width = 1280;   // For hardware JPEG init
        uint32_t height = 960;
    };

    /**
     * @param config  Still capture configuration.
     * @param raw_ring_buffer  Optional raw frame ring buffer for past-frame
     *                         retrieval.  May be nullptr if past stills are
     *                         not needed.
     */
    explicit StillCapture(const Config& config,
                          RawRingBuffer* raw_ring_buffer = nullptr);
    ~StillCapture();

    // Non-copyable
    StillCapture(const StillCapture&) = delete;
    StillCapture& operator=(const StillCapture&) = delete;

    /**
     * Start the capture worker thread.
     */
    bool start();

    /**
     * Stop the worker thread.
     */
    void stop();

    /**
     * Request a still capture.
     * Returns immediately; capture happens asynchronously.
     * @param time_offset_seconds negative=n seconds ago, positive=in n seconds, 0=now
     * @param burst_prefix Optional prefix for burst filenames (empty = normal "still_" prefix)
     * @return Request ID for tracking
     */
    uint64_t request_capture(int time_offset_seconds = 0, const std::string& burst_prefix = "");

    /**
     * Submit a frame for potential still capture.
     * Called by the capture pipeline for every frame.
     */
    void submit_frame(const uint8_t* data, size_t size, const FrameMetadata& metadata);

    /**
     * Wait for a capture to complete.
     * @param request_id The request ID from request_capture()
     * @param timeout_ms Maximum time to wait (-1 = infinite)
     * @return Path to the captured file, or empty on timeout/error
     */
    std::string wait_for_capture(uint64_t request_id, int timeout_ms = 5000);

    /**
     * Get statistics.
     */
    struct Stats {
        uint64_t captures_requested;
        uint64_t captures_completed;
        uint64_t captures_failed;
        uint64_t average_encode_time_us;
    };
    Stats get_stats() const;

private:
    void worker_thread_func();
    std::string encode_jpeg(const uint8_t* data, const FrameMetadata& metadata, const std::string& prefix);
    std::string encode_jpeg_hardware(const uint8_t* data, size_t size, const FrameMetadata& metadata, const std::string& prefix);

    Config config_;
    std::atomic<bool> running_{false};
    std::thread worker_thread_;
    
    // Optional raw frame ring buffer for past-frame stills
    RawRingBuffer* raw_ring_buffer_ = nullptr;

    // Hardware JPEG encoder (optional, falls back to libjpeg)
    std::unique_ptr<V4L2JpegEncoder> hw_jpeg_;

    // Pending capture requests
    struct StillRequest {
        uint64_t request_id;
        int time_offset_seconds;  // negative=past, positive=future, 0=now
        std::string filename_prefix;  // "still_" or "burst_NNN_"
    };
    mutable std::mutex request_mutex_;
    std::condition_variable request_cv_;
    std::atomic<uint64_t> next_request_id_{1};
    std::queue<StillRequest> pending_requests_;
    std::atomic<bool> capture_in_progress_{false};  // Track when worker is waiting for frame

    // Frame buffer for capture (lazy copy optimization)
    mutable std::mutex frame_mutex_;
    std::condition_variable frame_cv_;  // Notify when frame is available
    std::vector<uint8_t> latest_frame_;
    FrameMetadata latest_metadata_;
    const uint8_t* latest_frame_data_ptr_ = nullptr;  // Pointer for lazy copy
    size_t latest_frame_size_ = 0;
    bool frame_available_ = false;

    // Completed captures
    mutable std::mutex result_mutex_;
    std::condition_variable result_cv_;
    std::unordered_map<uint64_t, std::string> completed_captures_;

    // Statistics
    std::atomic<uint64_t> captures_requested_{0};
    std::atomic<uint64_t> captures_completed_{0};
    std::atomic<uint64_t> captures_failed_{0};
    std::atomic<uint64_t> total_encode_time_us_{0};
};

} // namespace camera_daemon
