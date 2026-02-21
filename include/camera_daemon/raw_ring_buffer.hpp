#pragma once

#include "common.hpp"
#include <vector>
#include <mutex>
#include <cstdint>
#include <cstring>

namespace camera_daemon {

/**
 * Thread-safe ring buffer for raw (unencoded) YUV420 frames.
 *
 * Stores the most recent N seconds of raw frames so that still capture
 * can retrieve a frame from the recent past.  Each slot is pre-allocated
 * to the maximum frame size to avoid per-frame heap allocation.
 */
class RawRingBuffer {
public:
    /**
     * @param max_duration_seconds  How many seconds of history to retain.
     * @param framerate             Expected frames per second.
     * @param max_frame_bytes       Maximum size of a single raw frame
     *                              (width * height * 3/2 for YUV420).
     */
    RawRingBuffer(size_t max_duration_seconds, uint32_t framerate,
                  size_t max_frame_bytes);

    ~RawRingBuffer() = default;

    // Non-copyable
    RawRingBuffer(const RawRingBuffer&) = delete;
    RawRingBuffer& operator=(const RawRingBuffer&) = delete;

    /**
     * Push a raw frame into the ring buffer.
     * Overwrites the oldest slot when full.
     */
    void push(const uint8_t* data, size_t size, const FrameMetadata& metadata);

    /**
     * Retrieve the frame whose timestamp is closest to the requested time.
     *
     * @param target_timestamp_us  Desired timestamp (microseconds since epoch).
     * @param[out] out_data        Pointer set to the internal frame data.
     *                             Valid only while the caller holds the lock
     *                             (see scoped_read below).
     * @param[out] out_size        Size of the frame in bytes.
     * @param[out] out_metadata    Frame metadata.
     * @return true if a frame was found, false if the buffer is empty.
     */
    bool find_nearest(uint64_t target_timestamp_us,
                      const uint8_t*& out_data,
                      size_t& out_size,
                      FrameMetadata& out_metadata) const;

    /**
     * Copy the frame whose timestamp is closest to the requested time
     * into the caller-supplied vector.  Thread-safe; no external locking
     * required.
     *
     * @return true if a frame was copied, false if the buffer is empty.
     */
    bool copy_nearest(uint64_t target_timestamp_us,
                      std::vector<uint8_t>& out_data,
                      FrameMetadata& out_metadata) const;

    /**
     * Get the timestamp range currently held.
     * @return {oldest_us, newest_us} or {0,0} if empty.
     */
    std::pair<uint64_t, uint64_t> time_range() const;

    /**
     * Number of frames currently stored.
     */
    size_t size() const;

    /**
     * Maximum number of frame slots.
     */
    size_t capacity() const { return slots_.size(); }

    /**
     * Clear all stored frames.
     */
    void clear();

    struct Stats {
        size_t frame_count;
        size_t total_bytes;
        size_t slot_capacity;
        uint64_t oldest_timestamp_us;
        uint64_t newest_timestamp_us;
    };
    Stats get_stats() const;

private:
    struct Slot {
        std::vector<uint8_t> data;   // Pre-allocated to max_frame_bytes
        FrameMetadata metadata{};
        size_t frame_size = 0;
        bool occupied = false;
    };

    mutable std::mutex mutex_;
    std::vector<Slot> slots_;
    size_t head_ = 0;       // Next write position
    size_t count_ = 0;      // Number of occupied slots
};

} // namespace camera_daemon
