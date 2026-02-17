#include "camera_daemon/ring_buffer.hpp"
#include "camera_daemon/logger.hpp"
#include <cstring>
#include <algorithm>

namespace camera_daemon {

EncodedRingBuffer::EncodedRingBuffer(size_t max_duration_seconds, uint32_t framerate)
    : max_frames_(max_duration_seconds * framerate)
    , max_duration_us_(max_duration_seconds * 1000000ULL) {
    LOG_DEBUG("Created ring buffer: max ", max_duration_seconds, "s, ", max_frames_, " frames");
}

void EncodedRingBuffer::push(EncodedFrame frame) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    total_bytes_ += frame.data.size();
    frames_.push_back(std::move(frame));
    
    evict_old_frames();
}

void EncodedRingBuffer::evict_old_frames() {
    // Remove frames exceeding max count
    while (frames_.size() > max_frames_) {
        total_bytes_ -= frames_.front().data.size();
        frames_.pop_front();
    }
    
    // Remove frames exceeding max duration
    if (!frames_.empty()) {
        uint64_t newest = frames_.back().metadata.timestamp_us;
        while (!frames_.empty() && 
               (newest - frames_.front().metadata.timestamp_us) > max_duration_us_) {
            total_bytes_ -= frames_.front().data.size();
            frames_.pop_front();
        }
    }
}

std::vector<EncodedFrame> EncodedRingBuffer::extract_last_seconds(size_t seconds) {
    std::lock_guard<std::mutex> lock(mutex_);
    
    if (frames_.empty()) {
        return {};
    }
    
    uint64_t cutoff_us = seconds * 1000000ULL;
    uint64_t newest_ts = frames_.back().metadata.timestamp_us;
    uint64_t target_ts = (newest_ts > cutoff_us) ? (newest_ts - cutoff_us) : 0;
    
    // Find the first keyframe at or before target timestamp
    auto start_it = frames_.end();
    for (auto it = frames_.begin(); it != frames_.end(); ++it) {
        if (it->metadata.timestamp_us >= target_ts) {
            // Go back to find the nearest keyframe
            auto keyframe_it = it;
            while (keyframe_it != frames_.begin() && !keyframe_it->metadata.is_keyframe) {
                --keyframe_it;
            }
            start_it = keyframe_it;
            break;
        }
    }
    
    if (start_it == frames_.end()) {
        // All frames are older than target, start from first keyframe
        for (auto it = frames_.begin(); it != frames_.end(); ++it) {
            if (it->metadata.is_keyframe) {
                start_it = it;
                break;
            }
        }
    }
    
    if (start_it == frames_.end()) {
        LOG_WARN("No keyframe found in ring buffer");
        return {};
    }
    
    // Pre-allocate result vector to avoid reallocations
    size_t count = std::distance(start_it, frames_.end());
    std::vector<EncodedFrame> result;
    result.reserve(count);
    
    for (auto it = start_it; it != frames_.end(); ++it) {
        result.push_back(*it);  // Copy frames (buffer must retain them)
    }

    LOG_DEBUG("Extracted ", result.size(), " frames from ring buffer");
    return result;
}

EncodedRingBuffer::Stats EncodedRingBuffer::get_stats() const {
    std::lock_guard<std::mutex> lock(mutex_);
    
    Stats stats{};
    stats.frame_count = frames_.size();
    stats.total_bytes = total_bytes_;
    
    if (!frames_.empty()) {
        stats.oldest_timestamp = frames_.front().metadata.timestamp_us;
        stats.newest_timestamp = frames_.back().metadata.timestamp_us;
        stats.duration_ms = (stats.newest_timestamp - stats.oldest_timestamp) / 1000;
    }
    
    return stats;
}

void EncodedRingBuffer::clear() {
    std::lock_guard<std::mutex> lock(mutex_);
    frames_.clear();
    total_bytes_ = 0;
}

} // namespace camera_daemon
