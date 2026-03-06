#pragma once

#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>

namespace camera_daemon {

/**
 * Reads audio samples from ws-audiod's POSIX shared memory region
 * and maintains a rolling buffer of raw PCM data with timestamps.
 *
 * ws-audiod SHM layout (all little-endian):
 *   [0..3]   uint32  magic  (0x41554449 = "AUDI")
 *   [4..7]   uint32  sample_rate
 *   [8..9]   uint16  channels
 *   [10..11] uint16  bits_per_sample
 *   [12..15] uint32  period_frames
 *   [16..23] uint64  write_counter  (monotonic, increments per period)
 *   [24..31] uint64  timestamp_us
 *   [32..63] reserved
 *   [64..]   sample data (interleaved int16_t, one period)
 */
class AudioReader {
public:
    static constexpr size_t HEADER_SIZE = 64;
    static constexpr uint32_t MAGIC = 0x41554449; // "AUDI"

    struct Config {
        std::string shm_name = "/ws_audiod_samples";
        uint32_t buffer_seconds = 60;  // How much audio to keep in memory
    };

    /// A timestamped chunk of raw PCM audio.
    struct AudioChunk {
        uint64_t timestamp_us;
        std::vector<uint8_t> data;   // Raw PCM (interleaved int16_t)
        uint32_t frame_count;
    };

    explicit AudioReader(const Config& config);
    ~AudioReader();

    AudioReader(const AudioReader&) = delete;
    AudioReader& operator=(const AudioReader&) = delete;

    bool start();
    void stop();
    bool is_running() const { return running_.load(); }

    /// Audio format (valid after start() connects to SHM).
    uint32_t sample_rate() const { return sample_rate_; }
    uint16_t channels() const { return channels_; }
    uint16_t bits_per_sample() const { return bits_per_sample_; }

    /**
     * Extract all audio covering a time range.
     * @param start_us  Start timestamp (microseconds since epoch)
     * @param end_us    End timestamp (microseconds since epoch)
     * @return Contiguous PCM data for the range, or empty if unavailable.
     */
    std::vector<uint8_t> extract_range(uint64_t start_us, uint64_t end_us) const;

    /**
     * Extract audio for the last N seconds.
     */
    std::vector<uint8_t> extract_last_seconds(uint32_t seconds) const;

    /**
     * Register a callback for real-time audio chunks (e.g. for RTSP streaming).
     * Called from the reader thread each time a new period arrives.
     * The callback must be fast and non-blocking.
     */
    using AudioChunkCallback = std::function<void(const AudioChunk&)>;
    void set_chunk_callback(AudioChunkCallback cb);

private:
    void reader_thread_func();
    bool connect_shm();
    void disconnect_shm();

    Config config_;

    // SHM state
    int shm_fd_ = -1;
    const uint8_t* shm_ptr_ = nullptr;
    size_t shm_size_ = 0;

    // Audio format (read from SHM header)
    uint32_t sample_rate_ = 0;
    uint16_t channels_ = 0;
    uint16_t bits_per_sample_ = 0;
    uint32_t period_frames_ = 0;

    // Reader thread
    std::atomic<bool> running_{false};
    std::thread reader_thread_;

    // Rolling buffer of audio chunks
    mutable std::mutex buffer_mutex_;
    std::deque<AudioChunk> chunks_;
    size_t max_chunks_ = 0;

    // Real-time callback
    AudioChunkCallback chunk_callback_;
};

} // namespace camera_daemon
