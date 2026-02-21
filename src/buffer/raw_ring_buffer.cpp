#include "camera_daemon/raw_ring_buffer.hpp"
#include "camera_daemon/logger.hpp"
#include <algorithm>
#include <cmath>

namespace camera_daemon {

RawRingBuffer::RawRingBuffer(size_t max_duration_seconds, uint32_t framerate,
                             size_t max_frame_bytes) {
    size_t num_slots = max_duration_seconds * framerate;
    if (num_slots == 0) {
        num_slots = 1;
    }

    slots_.resize(num_slots);
    for (auto& slot : slots_) {
        slot.data.resize(max_frame_bytes);
    }

    LOG_INFO("Raw ring buffer: ", num_slots, " slots, ",
             (num_slots * max_frame_bytes) / (1024 * 1024), " MB allocated");
}

void RawRingBuffer::push(const uint8_t* data, size_t size,
                         const FrameMetadata& metadata) {
    std::lock_guard<std::mutex> lock(mutex_);

    Slot& slot = slots_[head_];

    // Guard against frames larger than our pre-allocated slot
    size_t copy_size = std::min(size, slot.data.size());
    std::memcpy(slot.data.data(), data, copy_size);
    slot.metadata = metadata;
    slot.frame_size = copy_size;
    slot.occupied = true;

    head_ = (head_ + 1) % slots_.size();
    if (count_ < slots_.size()) {
        ++count_;
    }
}

bool RawRingBuffer::find_nearest(uint64_t target_timestamp_us,
                                 const uint8_t*& out_data,
                                 size_t& out_size,
                                 FrameMetadata& out_metadata) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (count_ == 0) {
        return false;
    }

    // Linear scan over occupied slots to find closest timestamp.
    // count_ is bounded by max_duration_seconds * framerate (typically 150),
    // so this is very fast.
    const Slot* best = nullptr;
    uint64_t best_diff = UINT64_MAX;

    for (size_t i = 0; i < count_; ++i) {
        // Walk backwards from head_ so we visit newest first
        size_t idx = (head_ + slots_.size() - 1 - i) % slots_.size();
        const Slot& s = slots_[idx];
        if (!s.occupied) continue;

        uint64_t diff = (s.metadata.timestamp_us >= target_timestamp_us)
                            ? (s.metadata.timestamp_us - target_timestamp_us)
                            : (target_timestamp_us - s.metadata.timestamp_us);
        if (diff < best_diff) {
            best_diff = diff;
            best = &s;
        }
    }

    if (!best) {
        return false;
    }

    out_data = best->data.data();
    out_size = best->frame_size;
    out_metadata = best->metadata;
    return true;
}

bool RawRingBuffer::copy_nearest(uint64_t target_timestamp_us,
                                 std::vector<uint8_t>& out_data,
                                 FrameMetadata& out_metadata) const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (count_ == 0) {
        return false;
    }

    const Slot* best = nullptr;
    uint64_t best_diff = UINT64_MAX;

    for (size_t i = 0; i < count_; ++i) {
        size_t idx = (head_ + slots_.size() - 1 - i) % slots_.size();
        const Slot& s = slots_[idx];
        if (!s.occupied) continue;

        uint64_t diff = (s.metadata.timestamp_us >= target_timestamp_us)
                            ? (s.metadata.timestamp_us - target_timestamp_us)
                            : (target_timestamp_us - s.metadata.timestamp_us);
        if (diff < best_diff) {
            best_diff = diff;
            best = &s;
        }
    }

    if (!best) {
        return false;
    }

    out_data.resize(best->frame_size);
    std::memcpy(out_data.data(), best->data.data(), best->frame_size);
    out_metadata = best->metadata;
    return true;
}

std::pair<uint64_t, uint64_t> RawRingBuffer::time_range() const {
    std::lock_guard<std::mutex> lock(mutex_);

    if (count_ == 0) {
        return {0, 0};
    }

    uint64_t oldest = UINT64_MAX;
    uint64_t newest = 0;

    for (size_t i = 0; i < count_; ++i) {
        size_t idx = (head_ + slots_.size() - 1 - i) % slots_.size();
        const Slot& s = slots_[idx];
        if (!s.occupied) continue;

        oldest = std::min(oldest, s.metadata.timestamp_us);
        newest = std::max(newest, s.metadata.timestamp_us);
    }

    return {oldest, newest};
}

size_t RawRingBuffer::size() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return count_;
}

void RawRingBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    for (auto& slot : slots_) {
        slot.occupied = false;
        slot.frame_size = 0;
    }
    head_ = 0;
    count_ = 0;
}

RawRingBuffer::Stats RawRingBuffer::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);

    Stats stats{};
    stats.slot_capacity = slots_.size();
    stats.frame_count = count_;

    for (size_t i = 0; i < count_; ++i) {
        size_t idx = (head_ + slots_.size() - 1 - i) % slots_.size();
        const Slot& s = slots_[idx];
        if (!s.occupied) continue;
        stats.total_bytes += s.frame_size;

        if (stats.oldest_timestamp_us == 0 ||
            s.metadata.timestamp_us < stats.oldest_timestamp_us) {
            stats.oldest_timestamp_us = s.metadata.timestamp_us;
        }
        if (s.metadata.timestamp_us > stats.newest_timestamp_us) {
            stats.newest_timestamp_us = s.metadata.timestamp_us;
        }
    }

    return stats;
}

} // namespace camera_daemon
