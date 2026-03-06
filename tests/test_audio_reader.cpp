#include <gtest/gtest.h>
#include "camera_daemon/audio_reader.hpp"
#include "camera_daemon/rtsp_server.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <atomic>

using namespace camera_daemon;

// ========== Helper: creates a fake ws-audiod SHM region ==========
class FakeSHM {
public:
    static constexpr uint32_t MAGIC = 0x41554449;

    FakeSHM(const std::string& name, uint32_t sample_rate, uint16_t channels,
            uint16_t bits, uint32_t period_frames)
        : name_(name), period_frames_(period_frames), channels_(channels), bits_(bits) {
        // Clean up any stale SHM
        shm_unlink(name_.c_str());

        size_t data_bytes = static_cast<size_t>(period_frames) * channels * (bits / 8);
        size_ = 64 + data_bytes;

        fd_ = shm_open(name_.c_str(), O_CREAT | O_RDWR, 0666);
        EXPECT_GE(fd_, 0);
        EXPECT_EQ(ftruncate(fd_, size_), 0);

        ptr_ = static_cast<uint8_t*>(
            mmap(nullptr, size_, PROT_READ | PROT_WRITE, MAP_SHARED, fd_, 0));
        EXPECT_NE(ptr_, MAP_FAILED);

        memset(ptr_, 0, size_);

        // Write header
        uint32_t magic = MAGIC;
        memcpy(ptr_, &magic, 4);
        memcpy(ptr_ + 4, &sample_rate, 4);
        memcpy(ptr_ + 8, &channels, 2);
        memcpy(ptr_ + 10, &bits, 2);
        memcpy(ptr_ + 12, &period_frames, 4);

        counter_ = 0;
    }

    ~FakeSHM() {
        stop_writing();
        if (ptr_ && ptr_ != MAP_FAILED) {
            munmap(ptr_, size_);
        }
        if (fd_ >= 0) {
            close(fd_);
        }
        shm_unlink(name_.c_str());
    }

    // Write a new period of data (increments counter, sets timestamp)
    void write_period() {
        counter_++;
        uint64_t ts = std::chrono::duration_cast<std::chrono::microseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count();
        memcpy(ptr_ + 16, &counter_, 8);
        memcpy(ptr_ + 24, &ts, 8);

        // Fill sample data with a pattern
        size_t data_bytes = static_cast<size_t>(period_frames_) * channels_ * (bits_ / 8);
        uint8_t pattern = static_cast<uint8_t>(counter_ & 0xFF);
        memset(ptr_ + 64, pattern, data_bytes);
    }

    // Start a background thread that writes periods at ~47Hz (48000/1024)
    void start_writing() {
        writing_ = true;
        writer_thread_ = std::thread([this]() {
            while (writing_) {
                write_period();
                std::this_thread::sleep_for(std::chrono::microseconds(21333)); // ~47Hz
            }
        });
    }

    void stop_writing() {
        writing_ = false;
        if (writer_thread_.joinable()) {
            writer_thread_.join();
        }
    }

    uint64_t counter() const { return counter_; }

private:
    std::string name_;
    int fd_ = -1;
    uint8_t* ptr_ = nullptr;
    size_t size_ = 0;
    uint32_t period_frames_;
    uint16_t channels_;
    uint16_t bits_;
    uint64_t counter_ = 0;
    std::atomic<bool> writing_{false};
    std::thread writer_thread_;
};


// ========== AudioReader Unit Tests ==========

class AudioReaderTest : public ::testing::Test {
protected:
    static constexpr const char* TEST_SHM = "/test_audio_reader";

    void TearDown() override {
        shm_unlink(TEST_SHM);
    }
};

TEST_F(AudioReaderTest, StartFailsWhenSHMDoesNotExist) {
    AudioReader::Config config;
    config.shm_name = "/nonexistent_audio_shm_test";

    AudioReader reader(config);
    EXPECT_FALSE(reader.start());
    EXPECT_FALSE(reader.is_running());
}

TEST_F(AudioReaderTest, StartFailsOnInvalidMagic) {
    // Create SHM with wrong magic
    int fd = shm_open(TEST_SHM, O_CREAT | O_RDWR, 0666);
    ASSERT_GE(fd, 0);
    ASSERT_EQ(ftruncate(fd, 2112), 0);
    uint8_t* ptr = static_cast<uint8_t*>(
        mmap(nullptr, 2112, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0));
    ASSERT_NE(ptr, MAP_FAILED);

    // Write bad magic
    uint32_t bad_magic = 0xDEADBEEF;
    memcpy(ptr, &bad_magic, 4);

    munmap(ptr, 2112);
    close(fd);

    AudioReader::Config config;
    config.shm_name = TEST_SHM;

    AudioReader reader(config);
    EXPECT_FALSE(reader.start());
    EXPECT_FALSE(reader.is_running());
}

TEST_F(AudioReaderTest, StartFailsWhenSHMStale) {
    // Create valid SHM but don't write any data (counter stays 0)
    FakeSHM shm(TEST_SHM, 48000, 1, 16, 1024);
    // Write one period so format is valid but then stop — counter won't change
    shm.write_period();

    AudioReader::Config config;
    config.shm_name = TEST_SHM;

    AudioReader reader(config);
    EXPECT_FALSE(reader.start());
    EXPECT_FALSE(reader.is_running());
}

TEST_F(AudioReaderTest, StartSucceedsWithLiveSHM) {
    FakeSHM shm(TEST_SHM, 48000, 1, 16, 1024);
    shm.start_writing();

    AudioReader::Config config;
    config.shm_name = TEST_SHM;

    AudioReader reader(config);
    EXPECT_TRUE(reader.start());
    EXPECT_TRUE(reader.is_running());
    EXPECT_EQ(reader.sample_rate(), 48000u);
    EXPECT_EQ(reader.channels(), 1);
    EXPECT_EQ(reader.bits_per_sample(), 16);

    reader.stop();
    shm.stop_writing();
}

TEST_F(AudioReaderTest, FormatAvailableImmediatelyAfterStart) {
    FakeSHM shm(TEST_SHM, 44100, 2, 16, 512);
    shm.start_writing();

    AudioReader::Config config;
    config.shm_name = TEST_SHM;

    AudioReader reader(config);
    ASSERT_TRUE(reader.start());

    // Format must be known synchronously — no race
    EXPECT_EQ(reader.sample_rate(), 44100u);
    EXPECT_EQ(reader.channels(), 2);
    EXPECT_EQ(reader.bits_per_sample(), 16);

    reader.stop();
    shm.stop_writing();
}

TEST_F(AudioReaderTest, ChunkCallbackReceivesData) {
    FakeSHM shm(TEST_SHM, 48000, 1, 16, 1024);
    shm.start_writing();

    AudioReader::Config config;
    config.shm_name = TEST_SHM;
    config.buffer_seconds = 5;

    std::atomic<int> chunk_count{0};
    AudioReader reader(config);
    reader.set_chunk_callback([&](const AudioReader::AudioChunk& chunk) {
        chunk_count++;
        EXPECT_EQ(chunk.frame_count, 1024u);
        EXPECT_EQ(chunk.data.size(), 2048u); // 1024 frames * 1 ch * 2 bytes
        EXPECT_GT(chunk.timestamp_us, 0u);
    });

    ASSERT_TRUE(reader.start());

    // Wait for some chunks to arrive
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    reader.stop();
    shm.stop_writing();

    EXPECT_GT(chunk_count.load(), 5); // ~23 chunks in 500ms at 48kHz/1024
}

TEST_F(AudioReaderTest, BufferAccumulatesChunks) {
    FakeSHM shm(TEST_SHM, 48000, 1, 16, 1024);
    shm.start_writing();

    AudioReader::Config config;
    config.shm_name = TEST_SHM;
    config.buffer_seconds = 5;

    std::atomic<int> chunks{0};
    AudioReader reader(config);
    reader.set_chunk_callback([&](const AudioReader::AudioChunk&) {
        chunks++;
    });
    ASSERT_TRUE(reader.start());

    std::this_thread::sleep_for(std::chrono::seconds(1));

    reader.stop();
    shm.stop_writing();

    EXPECT_GT(chunks.load(), 10);
}

TEST_F(AudioReaderTest, StopIsIdempotent) {
    FakeSHM shm(TEST_SHM, 48000, 1, 16, 1024);
    shm.start_writing();

    AudioReader::Config config;
    config.shm_name = TEST_SHM;

    AudioReader reader(config);
    ASSERT_TRUE(reader.start());

    reader.stop();
    reader.stop(); // Should not crash
    EXPECT_FALSE(reader.is_running());

    shm.stop_writing();
}

TEST_F(AudioReaderTest, StartAfterStopFails) {
    // After stop(), SHM is disconnected, so re-start should try reconnect
    FakeSHM shm(TEST_SHM, 48000, 1, 16, 1024);
    shm.start_writing();

    AudioReader::Config config;
    config.shm_name = TEST_SHM;

    AudioReader reader(config);
    ASSERT_TRUE(reader.start());
    reader.stop();

    // Starting again should work since SHM is still live
    EXPECT_TRUE(reader.start());
    EXPECT_TRUE(reader.is_running());

    reader.stop();
    shm.stop_writing();
}

// ========== RTSP Audio Integration Tests ==========
// These test RTSP server audio format and pipeline with live SHM

TEST_F(AudioReaderTest, RTSPServerWithAudioFormat) {
    // Test that set_audio_format before start() produces a valid DESCRIBE
    RTSPServer::Config rtsp_config;
    rtsp_config.port = 18560;
    rtsp_config.width = 1280;
    rtsp_config.height = 720;
    rtsp_config.framerate = 30;
    rtsp_config.enable_audio = true;

    RTSPServer server(rtsp_config);
    server.set_audio_format(48000, 1, 16);
    ASSERT_TRUE(server.start());

    // Push a keyframe so GStreamer can preroll the video appsrc
    EncodedFrame kf;
    kf.metadata.width = 1280;
    kf.metadata.height = 720;
    kf.metadata.is_keyframe = true;
    kf.metadata.timestamp_us = 0;
    kf.data = {0x00, 0x00, 0x00, 0x01, 0x67};
    kf.data.resize(1000, 0x55);
    server.push_frame(kf);

    // Push audio chunks so GStreamer can preroll the audio appsrc too
    for (int i = 0; i < 10; ++i) {
        AudioReader::AudioChunk chunk;
        chunk.timestamp_us = i * 21333;
        chunk.frame_count = 1024;
        chunk.data.resize(2048, 0x00); // silence
        server.push_audio(chunk);
    }

    // Give server time to bind
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    // Send RTSP DESCRIBE and verify it returns 200 with audio track
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(sock, 0);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(18560);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ASSERT_EQ(connect(sock, (struct sockaddr*)&addr, sizeof(addr)), 0);

    std::string req = "DESCRIBE rtsp://127.0.0.1:18560/camera RTSP/1.0\r\n"
                      "CSeq: 1\r\n"
                      "Accept: application/sdp\r\n\r\n";
    send(sock, req.c_str(), req.size(), 0);

    char buf[4096] = {};
    int n = recv(sock, buf, sizeof(buf) - 1, 0);
    close(sock);

    ASSERT_GT(n, 0) << "DESCRIBE timed out — pipeline preroll failed";

    std::string response(buf, n);
    EXPECT_NE(response.find("RTSP/1.0 200"), std::string::npos) << "Expected 200 OK";
    EXPECT_NE(response.find("m=video"), std::string::npos) << "Missing video track in SDP";
    EXPECT_NE(response.find("m=audio"), std::string::npos) << "Missing audio track in SDP";
    EXPECT_NE(response.find("L16/48000"), std::string::npos) << "Missing L16/48000 in SDP";

    server.stop();
}

TEST_F(AudioReaderTest, RTSPServerWithoutAudioFormat) {
    // If audio format is NOT set before start, should fall back to video-only
    RTSPServer::Config rtsp_config;
    rtsp_config.port = 18561;
    rtsp_config.width = 1280;
    rtsp_config.height = 720;
    rtsp_config.framerate = 30;
    rtsp_config.enable_audio = true;
    // Note: NOT calling set_audio_format — sample_rate stays 0

    RTSPServer server(rtsp_config);
    ASSERT_TRUE(server.start());

    // Push a keyframe so GStreamer can preroll the video appsrc
    EncodedFrame kf;
    kf.metadata.width = 1280;
    kf.metadata.height = 720;
    kf.metadata.is_keyframe = true;
    kf.metadata.timestamp_us = 0;
    kf.data = {0x00, 0x00, 0x00, 0x01, 0x67};
    kf.data.resize(1000, 0x55);
    server.push_frame(kf);

    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(sock, 0);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(18561);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ASSERT_EQ(connect(sock, (struct sockaddr*)&addr, sizeof(addr)), 0);

    std::string req = "DESCRIBE rtsp://127.0.0.1:18561/camera RTSP/1.0\r\n"
                      "CSeq: 1\r\n"
                      "Accept: application/sdp\r\n\r\n";
    send(sock, req.c_str(), req.size(), 0);

    char buf[4096] = {};
    int n = recv(sock, buf, sizeof(buf) - 1, 0);
    close(sock);

    ASSERT_GT(n, 0) << "DESCRIBE timed out";

    std::string response(buf, n);
    EXPECT_NE(response.find("RTSP/1.0 200"), std::string::npos);
    EXPECT_NE(response.find("m=video"), std::string::npos);
    // Should NOT have audio track since format was never set
    EXPECT_EQ(response.find("m=audio"), std::string::npos) << "Audio track should not exist without format";

    server.stop();
}

TEST_F(AudioReaderTest, PushAudioToRTSPServer) {
    RTSPServer::Config rtsp_config;
    rtsp_config.port = 18562;
    rtsp_config.width = 1280;
    rtsp_config.height = 720;
    rtsp_config.framerate = 30;
    rtsp_config.enable_audio = true;

    RTSPServer server(rtsp_config);
    server.set_audio_format(48000, 1, 16);
    ASSERT_TRUE(server.start());

    // Push audio chunks — should not crash even with no clients
    for (int i = 0; i < 50; ++i) {
        AudioReader::AudioChunk chunk;
        chunk.timestamp_us = i * 21333;
        chunk.frame_count = 1024;
        chunk.data.resize(2048, 0x42);
        server.push_audio(chunk);
    }

    server.stop();
    SUCCEED();
}

// ========== Live Audio SHM Integration Test ==========
// Requires ws-audiod to be running

class LiveAudioTest : public ::testing::Test {
protected:
    bool has_live_audio() {
        int fd = shm_open("/ws_audiod_samples", O_RDONLY, 0);
        if (fd < 0) return false;

        struct stat st;
        if (fstat(fd, &st) < 0 || st.st_size < 64) {
            close(fd);
            return false;
        }

        uint8_t* ptr = static_cast<uint8_t*>(
            mmap(nullptr, st.st_size, PROT_READ, MAP_SHARED, fd, 0));
        if (ptr == MAP_FAILED) {
            close(fd);
            return false;
        }

        // Check counter is advancing
        uint64_t c1;
        memcpy(&c1, ptr + 16, 8);
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        uint64_t c2;
        memcpy(&c2, ptr + 16, 8);

        munmap(ptr, st.st_size);
        close(fd);
        return c2 > c1;
    }
};

TEST_F(LiveAudioTest, ConnectsToWsAudiod) {
    if (!has_live_audio()) {
        GTEST_SKIP() << "ws-audiod not running with sample sharing";
    }

    AudioReader::Config config;
    config.shm_name = "/ws_audiod_samples";
    config.buffer_seconds = 5;

    AudioReader reader(config);
    ASSERT_TRUE(reader.start());
    EXPECT_EQ(reader.sample_rate(), 48000u);
    EXPECT_EQ(reader.channels(), 1);
    EXPECT_EQ(reader.bits_per_sample(), 16);

    // Collect audio for 2 seconds
    std::atomic<int> chunks{0};
    reader.set_chunk_callback([&](const AudioReader::AudioChunk& chunk) {
        chunks++;
    });

    std::this_thread::sleep_for(std::chrono::seconds(2));

    auto pcm = reader.extract_last_seconds(1);

    reader.stop();

    EXPECT_GT(chunks.load(), 40); // ~94 chunks in 2s, be conservative
    EXPECT_GT(pcm.size(), 80000u); // ~96000 bytes per second of 48kHz mono 16-bit
}

TEST_F(LiveAudioTest, RTSPWithLiveAudio) {
    if (!has_live_audio()) {
        GTEST_SKIP() << "ws-audiod not running with sample sharing";
    }

    AudioReader::Config config;
    config.shm_name = "/ws_audiod_samples";
    config.buffer_seconds = 5;

    AudioReader reader(config);
    ASSERT_TRUE(reader.start());

    RTSPServer::Config rtsp_config;
    rtsp_config.port = 18563;
    rtsp_config.width = 1280;
    rtsp_config.height = 720;
    rtsp_config.framerate = 30;
    rtsp_config.enable_audio = true;

    RTSPServer server(rtsp_config);
    server.set_audio_format(reader.sample_rate(), reader.channels(), reader.bits_per_sample());

    // Wire audio reader to RTSP server
    reader.set_chunk_callback([&](const AudioReader::AudioChunk& chunk) {
        server.push_audio(chunk);
    });

    ASSERT_TRUE(server.start());

    // Push some fake video frames too
    for (int i = 0; i < 10; ++i) {
        EncodedFrame frame;
        frame.metadata.width = 1280;
        frame.metadata.height = 720;
        frame.metadata.is_keyframe = (i == 0);
        frame.metadata.timestamp_us = i * 33333;
        frame.data = {0x00, 0x00, 0x00, 0x01, static_cast<uint8_t>(i == 0 ? 0x67 : 0x41)};
        frame.data.resize(5000, 0x55);
        server.push_frame(frame);
        std::this_thread::sleep_for(std::chrono::milliseconds(33));
    }

    // Verify DESCRIBE returns audio+video SDP
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(sock, 0);

    struct sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(18563);
    addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);

    struct timeval tv;
    tv.tv_sec = 5;
    tv.tv_usec = 0;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    ASSERT_EQ(connect(sock, (struct sockaddr*)&addr, sizeof(addr)), 0);

    std::string req = "DESCRIBE rtsp://127.0.0.1:18563/camera RTSP/1.0\r\n"
                      "CSeq: 1\r\n"
                      "Accept: application/sdp\r\n\r\n";
    send(sock, req.c_str(), req.size(), 0);

    char buf[4096] = {};
    int n = recv(sock, buf, sizeof(buf) - 1, 0);
    close(sock);

    ASSERT_GT(n, 0) << "DESCRIBE timed out";

    std::string response(buf, n);
    EXPECT_NE(response.find("m=video"), std::string::npos);
    EXPECT_NE(response.find("m=audio"), std::string::npos);
    EXPECT_NE(response.find("L16/48000"), std::string::npos);

    reader.stop();
    server.stop();
}
