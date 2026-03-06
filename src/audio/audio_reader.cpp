#include "camera_daemon/audio_reader.hpp"
#include "camera_daemon/logger.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <chrono>

namespace camera_daemon {

AudioReader::AudioReader(const Config& config)
    : config_(config) {}

AudioReader::~AudioReader() {
    stop();
}

bool AudioReader::connect_shm() {
    shm_fd_ = shm_open(config_.shm_name.c_str(), O_RDONLY, 0);
    if (shm_fd_ < 0) {
        LOG_WARN("Cannot open audio shared memory '", config_.shm_name,
                 "': ", strerror(errno),
                 " — is ws-audiod running with enable_sample_sharing=true?");
        return false;
    }

    struct stat st;
    if (fstat(shm_fd_, &st) < 0) {
        LOG_ERROR("Cannot stat audio SHM: ", strerror(errno));
        close(shm_fd_);
        shm_fd_ = -1;
        return false;
    }

    shm_size_ = static_cast<size_t>(st.st_size);
    if (shm_size_ < HEADER_SIZE) {
        LOG_ERROR("Audio SHM too small: ", shm_size_, " bytes");
        close(shm_fd_);
        shm_fd_ = -1;
        return false;
    }

    shm_ptr_ = static_cast<const uint8_t*>(
        mmap(nullptr, shm_size_, PROT_READ, MAP_SHARED, shm_fd_, 0));
    if (shm_ptr_ == MAP_FAILED) {
        LOG_ERROR("Cannot mmap audio SHM: ", strerror(errno));
        shm_ptr_ = nullptr;
        close(shm_fd_);
        shm_fd_ = -1;
        return false;
    }

    // Verify magic
    uint32_t magic;
    std::memcpy(&magic, shm_ptr_, 4);
    if (magic != MAGIC) {
        LOG_ERROR("Invalid audio SHM magic: 0x", std::hex, magic);
        disconnect_shm();
        return false;
    }

    // Read audio format from header
    std::memcpy(&sample_rate_,    shm_ptr_ + 4,  4);
    std::memcpy(&channels_,       shm_ptr_ + 8,  2);
    std::memcpy(&bits_per_sample_, shm_ptr_ + 10, 2);
    std::memcpy(&period_frames_,  shm_ptr_ + 12, 4);

    // Calculate how many chunks to keep
    if (period_frames_ > 0 && sample_rate_ > 0) {
        double periods_per_second = static_cast<double>(sample_rate_) / period_frames_;
        max_chunks_ = static_cast<size_t>(periods_per_second * config_.buffer_seconds) + 1;
    } else {
        max_chunks_ = 10000;  // Fallback
    }

    // Verify data is actively streaming by checking write counter changes
    uint64_t counter1;
    std::memcpy(&counter1, shm_ptr_ + 16, 8);
    std::this_thread::sleep_for(std::chrono::milliseconds(250));
    uint64_t counter2;
    std::memcpy(&counter2, shm_ptr_ + 16, 8);

    if (counter2 == counter1) {
        LOG_ERROR("Audio SHM '", config_.shm_name,
                  "' is not receiving data — is ws-audiod running?");
        disconnect_shm();
        return false;
    }

    LOG_INFO("Connected to audio SHM '", config_.shm_name, "': ",
             sample_rate_, " Hz, ", channels_, " ch, ",
             bits_per_sample_, " bit, period=", period_frames_, " frames");
    return true;
}

void AudioReader::disconnect_shm() {
    if (shm_ptr_ && shm_ptr_ != MAP_FAILED) {
        munmap(const_cast<uint8_t*>(shm_ptr_), shm_size_);
        shm_ptr_ = nullptr;
    }
    if (shm_fd_ >= 0) {
        close(shm_fd_);
        shm_fd_ = -1;
    }
}

bool AudioReader::start() {
    if (running_) return true;

    // Connect SHM synchronously so format is known before returning
    if (!connect_shm()) {
        return false;
    }

    running_ = true;
    reader_thread_ = std::thread(&AudioReader::reader_thread_func, this);
    return true;
}

void AudioReader::stop() {
    if (!running_) return;

    running_ = false;
    if (reader_thread_.joinable()) {
        reader_thread_.join();
    }
    disconnect_shm();
}

void AudioReader::reader_thread_func() {
    LOG_DEBUG("Audio reader thread started");

    // Connect if not already connected (from start())
    while (running_ && !shm_ptr_) {
        if (connect_shm()) break;
        std::this_thread::sleep_for(std::chrono::seconds(2));
    }

    if (!running_) return;

    uint64_t last_counter = 0;
    size_t bytes_per_period = static_cast<size_t>(period_frames_) *
                              channels_ * (bits_per_sample_ / 8);

    while (running_) {
        // Read write counter from SHM header
        uint64_t counter;
        std::memcpy(&counter, shm_ptr_ + 16, 8);

        if (counter != last_counter) {
            // New period available — read timestamp and data
            uint64_t timestamp_us;
            std::memcpy(&timestamp_us, shm_ptr_ + 24, 8);

            // Memory fence to ensure we see consistent data
            __sync_synchronize();

            AudioChunk chunk;
            chunk.timestamp_us = timestamp_us;
            chunk.frame_count = period_frames_;
            chunk.data.resize(bytes_per_period);
            std::memcpy(chunk.data.data(), shm_ptr_ + HEADER_SIZE, bytes_per_period);

            // Notify real-time listener (e.g. RTSP) before buffering
            if (chunk_callback_) {
                chunk_callback_(chunk);
            }

            {
                std::lock_guard<std::mutex> lock(buffer_mutex_);
                chunks_.push_back(std::move(chunk));

                // Evict old chunks
                while (chunks_.size() > max_chunks_) {
                    chunks_.pop_front();
                }
            }

            last_counter = counter;
        } else {
            // No new data — sleep briefly
            std::this_thread::sleep_for(std::chrono::microseconds(500));
        }
    }

    LOG_DEBUG("Audio reader thread stopped");
}

std::vector<uint8_t> AudioReader::extract_range(uint64_t start_us, uint64_t end_us) const {
    std::lock_guard<std::mutex> lock(buffer_mutex_);

    if (chunks_.empty()) return {};

    std::vector<uint8_t> result;
    for (const auto& chunk : chunks_) {
        if (chunk.timestamp_us >= start_us && chunk.timestamp_us <= end_us) {
            result.insert(result.end(), chunk.data.begin(), chunk.data.end());
        }
    }
    return result;
}

std::vector<uint8_t> AudioReader::extract_last_seconds(uint32_t seconds) const {
    uint64_t now_us = static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
    uint64_t start_us = now_us - (static_cast<uint64_t>(seconds) * 1000000ULL);
    return extract_range(start_us, now_us);
}

void AudioReader::set_chunk_callback(AudioChunkCallback cb) {
    chunk_callback_ = std::move(cb);
}

} // namespace camera_daemon
