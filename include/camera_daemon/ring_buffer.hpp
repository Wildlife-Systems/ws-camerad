#pragma once

#include "common.hpp"
#include <deque>
#include <mutex>
#include <condition_variable>
#include <optional>

namespace camera_daemon {

/**
 * Thread-safe ring buffer for encoded frames.
 * Used for pre-event video clip extraction.
 */
class EncodedRingBuffer {
public:
    explicit EncodedRingBuffer(size_t max_duration_seconds, uint32_t framerate);
    ~EncodedRingBuffer() = default;

    // Non-copyable
    EncodedRingBuffer(const EncodedRingBuffer&) = delete;
    EncodedRingBuffer& operator=(const EncodedRingBuffer&) = delete;

    /**
     * Push an encoded frame into the buffer.
     * Old frames are automatically evicted when capacity is exceeded.
     */
    void push(EncodedFrame frame);

    /**
     * Extract all frames from the last N seconds.
     * Returns frames starting from the most recent keyframe before the cutoff.
     */
    std::vector<EncodedFrame> extract_last_seconds(size_t seconds);

    /**
     * Get current buffer statistics.
     */
    struct Stats {
        size_t frame_count;
        size_t total_bytes;
        size_t duration_ms;
        uint64_t oldest_timestamp;
        uint64_t newest_timestamp;
    };
    Stats get_stats() const;

    /**
     * Get the maximum duration the buffer can hold.
     */
    size_t max_duration_seconds() const { return max_duration_us_ / 1000000ULL; }

    /**
     * Clear all frames.
     */
    void clear();

private:
    void evict_old_frames();

    mutable std::mutex mutex_;
    std::deque<EncodedFrame> frames_;
    size_t max_frames_;
    size_t max_duration_us_;
    size_t total_bytes_ = 0;
};

} // namespace camera_daemon
