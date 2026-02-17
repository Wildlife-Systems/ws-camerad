#pragma once

#include "common.hpp"
#include <string>
#include <atomic>

namespace camera_daemon {

/**
 * POSIX shared memory wrapper for frame distribution.
 */
class SharedMemory {
public:
    /**
     * Create or open shared memory.
     * @param name Shared memory name (e.g., "/camera_frames")
     * @param size Size in bytes
     * @param create If true, create new; if false, open existing
     */
    SharedMemory(const std::string& name, size_t size, bool create);
    ~SharedMemory();

    // Non-copyable, movable
    SharedMemory(const SharedMemory&) = delete;
    SharedMemory& operator=(const SharedMemory&) = delete;
    SharedMemory(SharedMemory&& other) noexcept;
    SharedMemory& operator=(SharedMemory&& other) noexcept;

    /**
     * Get pointer to mapped memory.
     */
    void* data() { return data_; }
    const void* data() const { return data_; }

    /**
     * Get size of mapped region.
     */
    size_t size() const { return size_; }

    /**
     * Check if mapping is valid.
     */
    bool is_valid() const { return data_ != nullptr; }

    /**
     * Get the shared memory name.
     */
    const std::string& name() const { return name_; }

    /**
     * Unlink (delete) the shared memory object.
     * Should only be called by the creator when done.
     */
    void unlink();

private:
    std::string name_;
    size_t size_;
    void* data_;
    int fd_;
    bool owner_;
};

/**
 * Frame publisher using shared memory.
 * Single producer, multiple consumers.
 */
class FramePublisher {
public:
    struct Header {
        std::atomic<uint64_t> sequence;
        std::atomic<uint32_t> frame_ready;
        uint32_t width;
        uint32_t height;
        uint32_t stride;
        uint32_t format;
        uint32_t frame_size;
        uint64_t timestamp_us;
        uint32_t is_keyframe;
        uint32_t reserved[4];
        // Frame data follows
    };

    FramePublisher(const std::string& shm_name, uint32_t width, uint32_t height, uint32_t format);
    ~FramePublisher();

    /**
     * Publish a frame to shared memory.
     */
    void publish(const uint8_t* data, size_t size, const FrameMetadata& metadata);

    /**
     * Get current sequence number.
     */
    uint64_t sequence() const;

private:
    std::unique_ptr<SharedMemory> shm_;
    Header* header_;
    uint8_t* frame_data_;
};

/**
 * Frame subscriber using shared memory.
 * Reads frames published by FramePublisher.
 */
class FrameSubscriber {
public:
    explicit FrameSubscriber(const std::string& shm_name);
    ~FrameSubscriber();

    /**
     * Wait for and read the next frame.
     * @param timeout_ms Maximum time to wait (0 = no wait, -1 = infinite)
     * @return true if frame was read, false on timeout
     */
    bool read_frame(std::vector<uint8_t>& data, FrameMetadata& metadata, int timeout_ms = -1);

    /**
     * Check if a new frame is available without blocking.
     */
    bool has_new_frame() const;

    /**
     * Get frame dimensions.
     */
    uint32_t width() const;
    uint32_t height() const;

private:
    std::unique_ptr<SharedMemory> shm_;
    const FramePublisher::Header* header_;
    const uint8_t* frame_data_;
    uint64_t last_sequence_;
};

} // namespace camera_daemon
