#pragma once

#include "common.hpp"
#include <linux/videodev2.h>
#include <thread>
#include <atomic>
#include <mutex>

namespace camera_daemon {

/**
 * Hardware H.264/H.265 encoder using V4L2 M2M API.
 * Uses the Raspberry Pi's hardware encoder.
 */
class V4L2Encoder {
public:
    enum class Codec {
        H264,
        H265
    };

    struct Config {
        uint32_t width;
        uint32_t height;
        uint32_t framerate;
        uint32_t bitrate;
        uint32_t keyframe_interval;
        Codec codec = Codec::H264;
        bool use_userptr = false;  // Use USERPTR instead of DMABUF (needed for software-rotated frames)
    };

    V4L2Encoder();
    ~V4L2Encoder();

    // Non-copyable
    V4L2Encoder(const V4L2Encoder&) = delete;
    V4L2Encoder& operator=(const V4L2Encoder&) = delete;

    /**
     * Initialize the encoder with given configuration.
     */
    bool initialize(const Config& config);

    /**
     * Start the encoder threads.
     */
    bool start();

    /**
     * Stop the encoder.
     */
    void stop();

    /**
     * Check if encoder is running.
     */
    bool is_running() const { return running_.load(); }

    /**
     * Check if encoder is in userptr/copy mode (vs DMABUF zero-copy).
     */
    bool is_userptr_mode() const { return config_.use_userptr; }

    /**
     * Submit a raw frame using DMA-BUF fd (zero-copy).
     * The dmabuf_fd must remain valid until the next frame is submitted.
     */
    bool encode_frame_dmabuf(int dmabuf_fd, size_t size, uint64_t timestamp);

    /**
     * Submit a raw frame from a userspace buffer (USERPTR).
     * Used when frames have been software-rotated and are no longer in a DMABUF.
     */
    bool encode_frame_userptr(const uint8_t* data, size_t size, uint64_t timestamp);

    /**
     * Register callback for encoded frames.
     */
    void set_output_callback(EncodedFrameCallback callback);

    /**
     * Force an IDR (keyframe) on next frame.
     */
    void force_keyframe();

    /**
     * Get encoder statistics.
     */
    struct Stats {
        uint64_t frames_in;
        uint64_t frames_out;
        uint64_t bytes_out;
        uint64_t dropped_frames;
    };
    Stats get_stats() const;

private:
    bool open_device();
    bool setup_input_format();
    bool setup_output_format();
    bool setup_controls();
    bool allocate_buffers();
    bool start_streaming();

    void output_thread_func();
    void dequeue_output_buffer();

    int fd_ = -1;
    Config config_;
    EncodedFrameCallback output_callback_;

    std::atomic<bool> running_{false};
    std::atomic<bool> force_keyframe_{false};

    // Input buffer tracking (DMABUF zero-copy, or MMAP for software rotation)
    struct InputBuffer {
        bool queued;
        void* data = nullptr;       // MMAP pointer (only when use_userptr/mmap_input is true)
        size_t capacity = 0;        // Size of mapped region
    };
    std::vector<InputBuffer> input_buffers_;
    std::mutex input_mutex_;

    // Output (encoded) buffers
    struct OutputBuffer {
        void* data;
        size_t size;
        size_t capacity;
        bool queued;
    };
    std::vector<OutputBuffer> output_buffers_;

    std::thread output_thread_;

    // Statistics
    std::atomic<uint64_t> frames_in_{0};
    std::atomic<uint64_t> frames_out_{0};
    std::atomic<uint64_t> bytes_out_{0};
    std::atomic<uint64_t> dropped_frames_{0};
};

} // namespace camera_daemon
