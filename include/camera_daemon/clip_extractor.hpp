#pragma once

#include "common.hpp"
#include "ring_buffer.hpp"
#include "audio_reader.hpp"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <condition_variable>
#include <queue>
#include <vector>
#include <list>
#include <unordered_map>
#include <memory>

namespace camera_daemon {

/**
 * Extracts video clips from the ring buffer.
 * Supports pre-event and post-event recording.
 * Multiple clips can be extracted concurrently — each clip request
 * gets its own worker thread so overlapping post-event windows work.
 */
class ClipExtractor {
public:
    struct Config {
        std::string output_dir = DEFAULT_CLIPS_DIR;
        uint32_t pre_event_seconds = 10;
        uint32_t post_event_seconds = 5;
        bool remux_to_mp4 = true;
        uint32_t max_concurrent = 4;  // Max simultaneous clip extractions
    };

    struct ClipRequest {
        uint64_t request_id;
        int start_offset;        // Start time relative to request (negative=past)
        int end_offset;          // End time relative to request (positive=future)
        uint64_t request_timestamp;  // When the request was made
    };

    explicit ClipExtractor(const Config& config, EncodedRingBuffer& ring_buffer,
                           AudioReader* audio_reader = nullptr);
    ~ClipExtractor();

    // Non-copyable
    ClipExtractor(const ClipExtractor&) = delete;
    ClipExtractor& operator=(const ClipExtractor&) = delete;

    /**
     * Start the extractor dispatcher thread.
     */
    bool start();

    /**
     * Stop the dispatcher and all worker threads.
     */
    void stop();

    /**
     * Request a clip extraction.
     * @param start_offset Start time (negative=past, positive=future)
     * @param end_offset End time (negative=past, positive=future)
     * @return Request ID for tracking
     */
    uint64_t request_clip(int start_offset = -5, int end_offset = 5);

    /**
     * Add an encoded frame to the ring buffer.
     * Called by the capture pipeline.
     * Also feeds any active post-event recordings.
     */
    void add_frame(EncodedFrame frame);

    /**
     * Wait for a clip extraction to complete.
     * @param request_id The request ID from request_clip()
     * @param timeout_ms Maximum time to wait
     * @return Path to the extracted clip, or empty on error
     */
    std::string wait_for_clip(uint64_t request_id, int timeout_ms = 30000);

    /**
     * Check if any clip extraction is in progress.
     */
    bool is_extracting() const { return active_extractions_.load() > 0; }

    /**
     * Get statistics.
     */
    struct Stats {
        uint64_t clips_requested;
        uint64_t clips_completed;
        uint64_t clips_failed;
        uint64_t total_bytes_written;
    };
    Stats get_stats() const;

private:
    /**
     * Dispatcher thread: pops requests and spawns worker threads.
     */
    void dispatcher_thread_func();

    /**
     * Worker function for a single clip extraction (runs in its own thread).
     */
    void clip_worker_func(ClipRequest request);

    std::string extract_clip(const ClipRequest& request);
    std::string write_raw_h264(const std::vector<EncodedFrame>& frames, const std::string& base_path);
    std::string write_mp4(const std::vector<EncodedFrame>& frames, const std::string& base_path);

    Config config_;
    EncodedRingBuffer& ring_buffer_;
    AudioReader* audio_reader_ = nullptr;

    std::atomic<bool> running_{false};
    std::thread dispatcher_thread_;
    std::atomic<uint32_t> active_extractions_{0};

    // Request queue (dispatcher reads from here)
    mutable std::mutex request_mutex_;
    std::condition_variable request_cv_;
    std::queue<ClipRequest> pending_requests_;
    std::atomic<uint64_t> next_request_id_{1};

    // Active post-event recordings
    // Each active clip that needs post-event frames gets an entry here.
    struct ActiveRecording {
        uint64_t request_id;
        uint64_t end_timestamp;
        std::vector<EncodedFrame> frames;
        std::mutex mutex;
        std::condition_variable cv;
        bool done = false;
    };
    mutable std::mutex recordings_mutex_;
    std::list<std::shared_ptr<ActiveRecording>> active_recordings_;

    // Worker threads for clip extraction
    std::mutex workers_mutex_;
    std::vector<std::thread> clip_workers_;

    // Completed extractions
    mutable std::mutex result_mutex_;
    std::condition_variable result_cv_;
    std::unordered_map<uint64_t, std::string> completed_clips_;

    // Statistics
    std::atomic<uint64_t> clips_requested_{0};
    std::atomic<uint64_t> clips_completed_{0};
    std::atomic<uint64_t> clips_failed_{0};
    std::atomic<uint64_t> total_bytes_written_{0};
};

} // namespace camera_daemon
