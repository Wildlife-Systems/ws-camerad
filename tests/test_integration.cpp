#include <gtest/gtest.h>
#include "camera_daemon/still_capture.hpp"
#include "camera_daemon/clip_extractor.hpp"
#include <filesystem>
#include <fstream>
#include <cstring>

using namespace camera_daemon;

// ========== StillCapture Integration Tests ==========

class StillCaptureIntegrationTest : public ::testing::Test {
protected:
    std::string temp_dir;
    
    void SetUp() override {
        temp_dir = "/tmp/still_test_" + std::to_string(getpid());
        std::filesystem::create_directories(temp_dir);
    }
    
    void TearDown() override {
        std::filesystem::remove_all(temp_dir);
    }
    
    // Create test YUV420 frame data
    std::vector<uint8_t> create_test_yuv_frame(uint32_t width, uint32_t height) {
        size_t size = width * height * 3 / 2;
        std::vector<uint8_t> data(size);
        
        // Y plane: gradient
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                data[y * width + x] = static_cast<uint8_t>((x + y) % 256);
            }
        }
        // U and V: neutral (128)
        memset(data.data() + width * height, 128, width * height / 2);
        
        return data;
    }
    
    FrameMetadata create_test_metadata(uint32_t width, uint32_t height) {
        FrameMetadata meta{};
        meta.width = width;
        meta.height = height;
        meta.stride = width;
        meta.size = width * height * 3 / 2;
        meta.timestamp_us = get_timestamp_us();
        meta.format = PIXFMT_YUV420;
        return meta;
    }
};

TEST_F(StillCaptureIntegrationTest, StartStop) {
    StillCapture::Config config;
    config.output_dir = temp_dir;
    config.width = 640;
    config.height = 480;
    
    StillCapture capture(config);
    
    EXPECT_TRUE(capture.start());
    capture.stop();
}

TEST_F(StillCaptureIntegrationTest, DoubleStartStop) {
    StillCapture::Config config;
    config.output_dir = temp_dir;
    config.width = 640;
    config.height = 480;
    
    StillCapture capture(config);
    
    EXPECT_TRUE(capture.start());
    EXPECT_TRUE(capture.start());  // Second start OK
    capture.stop();
    capture.stop();  // Second stop OK
}

TEST_F(StillCaptureIntegrationTest, RequestCaptureReturnsUniqueIds) {
    StillCapture::Config config;
    config.output_dir = temp_dir;
    config.width = 640;
    config.height = 480;
    
    StillCapture capture(config);
    capture.start();
    
    uint64_t id1 = capture.request_capture();
    uint64_t id2 = capture.request_capture();
    uint64_t id3 = capture.request_capture();
    
    EXPECT_NE(id1, id2);
    EXPECT_NE(id2, id3);
    EXPECT_NE(id1, id3);
    
    capture.stop();
}

TEST_F(StillCaptureIntegrationTest, StatsTracksRequests) {
    StillCapture::Config config;
    config.output_dir = temp_dir;
    config.width = 640;
    config.height = 480;
    
    StillCapture capture(config);
    capture.start();
    
    capture.request_capture();
    capture.request_capture();
    capture.request_capture();
    
    auto stats = capture.get_stats();
    EXPECT_EQ(stats.captures_requested, 3);
    
    capture.stop();
}

TEST_F(StillCaptureIntegrationTest, CaptureProducesJpegFile) {
    StillCapture::Config config;
    config.output_dir = temp_dir;
    config.jpeg_quality = 85;
    config.width = 640;
    config.height = 480;
    
    StillCapture capture(config);  
    ASSERT_TRUE(capture.start());
    
    // Request capture
    uint64_t id = capture.request_capture();
    
    // Submit a frame
    auto yuv_data = create_test_yuv_frame(640, 480);
    auto metadata = create_test_metadata(640, 480);
    capture.submit_frame(yuv_data.data(), yuv_data.size(), metadata);
    
    // Wait for result
    std::string path = capture.wait_for_capture(id, 5000);
    
    capture.stop();
    
    // Verify file was created
    ASSERT_FALSE(path.empty()) << "Capture should return a path";
    EXPECT_TRUE(std::filesystem::exists(path)) << "File should exist: " << path;
    
    // Verify it's a valid JPEG (check magic bytes)
    std::ifstream file(path, std::ios::binary);
    ASSERT_TRUE(file.good());
    
    uint8_t magic[2];
    file.read(reinterpret_cast<char*>(magic), 2);
    EXPECT_EQ(magic[0], 0xFF);
    EXPECT_EQ(magic[1], 0xD8);  // JPEG SOI marker
}

TEST_F(StillCaptureIntegrationTest, MultipleCaptures) {
    StillCapture::Config config;
    config.output_dir = temp_dir;
    config.width = 640;
    config.height = 480;
    
    StillCapture capture(config);
    ASSERT_TRUE(capture.start());
    
    std::vector<uint64_t> ids;
    
    // Request multiple captures
    for (int i = 0; i < 5; ++i) {
        ids.push_back(capture.request_capture());
    }
    
    // Submit frames
    auto yuv_data = create_test_yuv_frame(640, 480);
    auto metadata = create_test_metadata(640, 480);
    
    for (int i = 0; i < 10; ++i) {  // Submit more frames than requests
        metadata.timestamp_us = get_timestamp_us();
        capture.submit_frame(yuv_data.data(), yuv_data.size(), metadata);
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    
    // Wait for all captures
    int completed = 0;
    for (uint64_t id : ids) {
        std::string path = capture.wait_for_capture(id, 2000);
        if (!path.empty() && std::filesystem::exists(path)) {
            completed++;
        }
    }
    
    capture.stop();
    
    EXPECT_EQ(completed, 5);
    
    auto stats = capture.get_stats();
    EXPECT_EQ(stats.captures_completed, 5);
}

TEST_F(StillCaptureIntegrationTest, WaitTimeoutOnNoFrame) {
    StillCapture::Config config;
    config.output_dir = temp_dir;
    config.width = 640;
    config.height = 480;
    
    StillCapture capture(config);
    ASSERT_TRUE(capture.start());
    
    uint64_t id = capture.request_capture();
    
    // Don't submit any frames - should timeout
    std::string path = capture.wait_for_capture(id, 600);  // Short timeout
    
    capture.stop();
    
    // Should timeout and return empty (or failed capture)
    // The capture will fail due to no frame available
}

TEST_F(StillCaptureIntegrationTest, HardwareJpegIfAvailable) {
    if (!V4L2JpegEncoder::is_available()) {
        GTEST_SKIP() << "Hardware JPEG not available";
    }
    
    StillCapture::Config config;
    config.output_dir = temp_dir;
    config.jpeg_quality = 85;
    config.width = 1280;
    config.height = 960;
    
    StillCapture capture(config);
    ASSERT_TRUE(capture.start());
    
    uint64_t id = capture.request_capture();
    
    auto yuv_data = create_test_yuv_frame(1280, 960);
    auto metadata = create_test_metadata(1280, 960);
    capture.submit_frame(yuv_data.data(), yuv_data.size(), metadata);
    
    std::string path = capture.wait_for_capture(id, 5000);
    
    capture.stop();
    
    ASSERT_FALSE(path.empty());
    EXPECT_TRUE(std::filesystem::exists(path));
    
    auto stats = capture.get_stats();
    // Should complete faster with hardware (<50ms typically)
    EXPECT_LT(stats.average_encode_time_us, 100000);  // < 100ms
}

// ========== ClipExtractor Integration Tests ==========

class ClipExtractorIntegrationTest : public ::testing::Test {
protected:
    std::string temp_dir;
    std::unique_ptr<EncodedRingBuffer> ring_buffer;
    
    void SetUp() override {
        temp_dir = "/tmp/clip_test_" + std::to_string(getpid());
        std::filesystem::create_directories(temp_dir);
        
        // Create ring buffer (30 seconds at 30fps)
        ring_buffer = std::make_unique<EncodedRingBuffer>(30, 30);
    }
    
    void TearDown() override {
        std::filesystem::remove_all(temp_dir);
    }
    
    EncodedFrame create_test_h264_frame(uint64_t timestamp, bool keyframe) {
        EncodedFrame frame;
        frame.metadata.timestamp_us = timestamp;
        frame.metadata.is_keyframe = keyframe;
        frame.metadata.width = 1280;
        frame.metadata.height = 720;
        
        // Fake H.264 NAL unit (not valid H.264, just test data)
        if (keyframe) {
            // SPS NAL
            frame.data = {0x00, 0x00, 0x00, 0x01, 0x67, 0x01, 0x02, 0x03};
        } else {
            // P-frame NAL
            frame.data = {0x00, 0x00, 0x00, 0x01, 0x41, 0x01, 0x02, 0x03};
        }
        
        // Pad to realistic size
        frame.data.resize(10000, 0x55);
        
        return frame;
    }
};

TEST_F(ClipExtractorIntegrationTest, StartStop) {
    ClipExtractor::Config config;
    config.output_dir = temp_dir;
    
    ClipExtractor extractor(config, *ring_buffer);
    
    EXPECT_TRUE(extractor.start());
    EXPECT_FALSE(extractor.is_extracting());
    
    extractor.stop();
}

TEST_F(ClipExtractorIntegrationTest, RequestClipReturnsUniqueIds) {
    ClipExtractor::Config config;
    config.output_dir = temp_dir;
    
    ClipExtractor extractor(config, *ring_buffer);
    extractor.start();
    
    uint64_t id1 = extractor.request_clip(-5, 3);
    uint64_t id2 = extractor.request_clip(-5, 3);
    uint64_t id3 = extractor.request_clip(-5, 3);
    
    EXPECT_NE(id1, id2);
    EXPECT_NE(id2, id3);
    
    extractor.stop();
}

TEST_F(ClipExtractorIntegrationTest, StatsTracksRequests) {
    ClipExtractor::Config config;
    config.output_dir = temp_dir;
    
    ClipExtractor extractor(config, *ring_buffer);
    extractor.start();
    
    extractor.request_clip();
    extractor.request_clip();
    
    auto stats = extractor.get_stats();
    EXPECT_EQ(stats.clips_requested, 2);
    
    extractor.stop();
}

TEST_F(ClipExtractorIntegrationTest, AddFrameToRingBuffer) {
    ClipExtractor::Config config;
    config.output_dir = temp_dir;
    
    ClipExtractor extractor(config, *ring_buffer);
    extractor.start();
    
    uint64_t base_ts = get_timestamp_us();
    
    // Add 30 frames (1 second at 30fps)
    for (int i = 0; i < 30; ++i) {
        auto frame = create_test_h264_frame(base_ts + i * 33333, i == 0);
        extractor.add_frame(std::move(frame));
    }
    
    auto stats = ring_buffer->get_stats();
    EXPECT_EQ(stats.frame_count, 30);
    
    extractor.stop();
}

TEST_F(ClipExtractorIntegrationTest, ExtractClipWithPreEventFrames) {
    ClipExtractor::Config config;
    config.output_dir = temp_dir;
    config.remux_to_mp4 = false;  // Keep as raw H.264 for simplicity
    config.pre_event_seconds = 5;
    config.post_event_seconds = 0;
    
    ClipExtractor extractor(config, *ring_buffer);
    extractor.start();
    
    uint64_t base_ts = get_timestamp_us();
    
    // Add 10 seconds of frames (enough for 5s pre-event)
    for (int i = 0; i < 300; ++i) {  // 30fps * 10s
        auto frame = create_test_h264_frame(base_ts + i * 33333, i % 30 == 0);
        extractor.add_frame(std::move(frame));
    }
    
    // Request clip with only pre-event (no post-event wait)
    uint64_t id = extractor.request_clip(-5, 0);
    
    // Wait for extraction
    std::string path = extractor.wait_for_clip(id, 5000);
    
    extractor.stop();
    
    ASSERT_FALSE(path.empty()) << "Should return clip path";
    EXPECT_TRUE(std::filesystem::exists(path)) << "File should exist: " << path;
    
    // Check file has content
    auto size = std::filesystem::file_size(path);
    EXPECT_GT(size, 0);
}

TEST_F(ClipExtractorIntegrationTest, ExtractClipWithPostEvent) {
    ClipExtractor::Config config;
    config.output_dir = temp_dir;
    config.remux_to_mp4 = false;
    config.pre_event_seconds = 2;
    config.post_event_seconds = 2;
    
    ClipExtractor extractor(config, *ring_buffer);
    extractor.start();
    
    uint64_t base_ts = get_timestamp_us();
    
    // Add 5 seconds of pre-event frames
    for (int i = 0; i < 150; ++i) {
        auto frame = create_test_h264_frame(base_ts + i * 33333, i % 30 == 0);
        extractor.add_frame(std::move(frame));
    }
    
    // Request clip
    uint64_t id = extractor.request_clip(-2, 2);
    
    // Simulate post-event frames arriving
    std::thread frame_thread([&]() {
        for (int i = 150; i < 220; ++i) {
            auto frame = create_test_h264_frame(base_ts + i * 33333, i % 30 == 0);
            extractor.add_frame(std::move(frame));
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    });
    
    // Wait for extraction (needs to wait for post-event frames)
    std::string path = extractor.wait_for_clip(id, 10000);
    
    frame_thread.join();
    extractor.stop();
    
    ASSERT_FALSE(path.empty());
    EXPECT_TRUE(std::filesystem::exists(path));
    
    auto stats = extractor.get_stats();
    EXPECT_EQ(stats.clips_completed, 1);
}

TEST_F(ClipExtractorIntegrationTest, ConcurrentPreEventClips) {
    ClipExtractor::Config config;
    config.output_dir = temp_dir;
    config.remux_to_mp4 = false;
    config.pre_event_seconds = 5;
    config.post_event_seconds = 0;
    
    ClipExtractor extractor(config, *ring_buffer);
    extractor.start();
    
    uint64_t base_ts = get_timestamp_us();
    
    // Add 10 seconds of frames
    for (int i = 0; i < 300; ++i) {
        auto frame = create_test_h264_frame(base_ts + i * 33333, i % 30 == 0);
        extractor.add_frame(std::move(frame));
    }
    
    // Request 3 clips simultaneously (pre-event only, no blocking post-event)
    uint64_t id1 = extractor.request_clip(-5, 0);
    uint64_t id2 = extractor.request_clip(-3, 0);
    uint64_t id3 = extractor.request_clip(-5, 0);
    
    // Wait for all three
    std::string path1 = extractor.wait_for_clip(id1, 5000);
    std::string path2 = extractor.wait_for_clip(id2, 5000);
    std::string path3 = extractor.wait_for_clip(id3, 5000);
    
    extractor.stop();
    
    ASSERT_FALSE(path1.empty()) << "Clip 1 should succeed";
    ASSERT_FALSE(path2.empty()) << "Clip 2 should succeed";
    ASSERT_FALSE(path3.empty()) << "Clip 3 should succeed";
    
    EXPECT_TRUE(std::filesystem::exists(path1));
    EXPECT_TRUE(std::filesystem::exists(path2));
    EXPECT_TRUE(std::filesystem::exists(path3));
    
    // All three should be different files
    EXPECT_NE(path1, path2);
    EXPECT_NE(path2, path3);
    EXPECT_NE(path1, path3);
    
    auto stats = extractor.get_stats();
    EXPECT_EQ(stats.clips_requested, 3);
    EXPECT_EQ(stats.clips_completed, 3);
}

TEST_F(ClipExtractorIntegrationTest, ConcurrentPostEventClips) {
    ClipExtractor::Config config;
    config.output_dir = temp_dir;
    config.remux_to_mp4 = false;
    config.pre_event_seconds = 2;
    config.post_event_seconds = 2;
    
    ClipExtractor extractor(config, *ring_buffer);
    extractor.start();
    
    uint64_t base_ts = get_timestamp_us();
    
    // Add pre-event frames
    for (int i = 0; i < 150; ++i) {
        auto frame = create_test_h264_frame(base_ts + i * 33333, i % 30 == 0);
        extractor.add_frame(std::move(frame));
    }
    
    // Request 2 overlapping clips with post-event recording
    uint64_t id1 = extractor.request_clip(-2, 2);
    
    // Small delay then request second clip (overlapping post-event windows)
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    uint64_t id2 = extractor.request_clip(-2, 2);
    
    // Feed post-event frames for both clips
    std::thread frame_thread([&]() {
        for (int i = 150; i < 300; ++i) {
            auto frame = create_test_h264_frame(base_ts + i * 33333, i % 30 == 0);
            extractor.add_frame(std::move(frame));
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    });
    
    // Wait for both clips
    std::string path1 = extractor.wait_for_clip(id1, 15000);
    std::string path2 = extractor.wait_for_clip(id2, 15000);
    
    frame_thread.join();
    extractor.stop();
    
    ASSERT_FALSE(path1.empty()) << "Clip 1 should succeed";
    ASSERT_FALSE(path2.empty()) << "Clip 2 should succeed";
    
    EXPECT_TRUE(std::filesystem::exists(path1));
    EXPECT_TRUE(std::filesystem::exists(path2));
    EXPECT_NE(path1, path2);
    
    auto stats = extractor.get_stats();
    EXPECT_EQ(stats.clips_requested, 2);
    EXPECT_EQ(stats.clips_completed, 2);
}

TEST_F(ClipExtractorIntegrationTest, ConcurrentClipsDontBlock) {
    ClipExtractor::Config config;
    config.output_dir = temp_dir;
    config.remux_to_mp4 = false;
    config.pre_event_seconds = 2;
    config.post_event_seconds = 3;
    
    ClipExtractor extractor(config, *ring_buffer);
    extractor.start();
    
    uint64_t base_ts = get_timestamp_us();
    
    // Add pre-event frames
    for (int i = 0; i < 90; ++i) {
        auto frame = create_test_h264_frame(base_ts + i * 33333, i % 30 == 0);
        extractor.add_frame(std::move(frame));
    }
    
    // Request a clip with 3s post-event (will take ~3s to complete)
    uint64_t id1 = extractor.request_clip(-2, 3);
    
    // Immediately request a pre-event-only clip — should NOT be blocked by clip 1
    uint64_t id2 = extractor.request_clip(-2, 0);
    
    // Feed frames in background
    std::thread frame_thread([&]() {
        for (int i = 90; i < 250; ++i) {
            auto frame = create_test_h264_frame(base_ts + i * 33333, i % 30 == 0);
            extractor.add_frame(std::move(frame));
            std::this_thread::sleep_for(std::chrono::milliseconds(33));
        }
    });
    
    // Clip 2 (pre-event only) should complete much faster than clip 1
    auto start = std::chrono::steady_clock::now();
    std::string path2 = extractor.wait_for_clip(id2, 5000);
    auto clip2_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - start).count();
    
    // Clip 2 should finish in under 1 second (it's pre-event only)
    EXPECT_LT(clip2_time, 1000) << "Pre-event clip should not be blocked by post-event clip";
    ASSERT_FALSE(path2.empty()) << "Pre-event clip should succeed immediately";
    
    // Now wait for clip 1 (has 3s post-event)
    std::string path1 = extractor.wait_for_clip(id1, 15000);
    
    frame_thread.join();
    extractor.stop();
    
    ASSERT_FALSE(path1.empty()) << "Post-event clip should succeed";
    EXPECT_TRUE(std::filesystem::exists(path1));
    EXPECT_TRUE(std::filesystem::exists(path2));
    
    auto stats = extractor.get_stats();
    EXPECT_EQ(stats.clips_completed, 2);
}
// Test: requesting pre_seconds > buffer capacity gets clamped
TEST_F(ClipExtractorIntegrationTest, PreSecondsClampedToBufferCapacity) {
    ClipExtractor::Config config;
    config.output_dir = temp_dir;
    config.remux_to_mp4 = false;
    config.pre_event_seconds = 5;
    config.post_event_seconds = 0;
    
    // ring_buffer has 30s capacity
    ClipExtractor extractor(config, *ring_buffer);
    extractor.start();
    
    uint64_t base_ts = get_timestamp_us();
    
    // Add 10 seconds of frames
    for (int i = 0; i < 300; ++i) {
        auto frame = create_test_h264_frame(base_ts + i * 33333, i % 30 == 0);
        extractor.add_frame(std::move(frame));
    }
    
    // Request 60 seconds — should be clamped to 30 (buffer capacity)
    uint64_t id = extractor.request_clip(-60, 0);
    std::string path = extractor.wait_for_clip(id, 5000);
    
    extractor.stop();
    
    // Clip should still succeed (clamped, not rejected)
    ASSERT_FALSE(path.empty()) << "Over-capacity request should succeed (clamped)";
    EXPECT_TRUE(std::filesystem::exists(path));
    
    auto stats = extractor.get_stats();
    EXPECT_EQ(stats.clips_completed, 1);
}

// Test: requesting pre_seconds when buffer is still filling
TEST_F(ClipExtractorIntegrationTest, BufferStillFilling) {
    ClipExtractor::Config config;
    config.output_dir = temp_dir;
    config.remux_to_mp4 = false;
    config.pre_event_seconds = 10;
    config.post_event_seconds = 0;
    
    ClipExtractor extractor(config, *ring_buffer);
    extractor.start();
    
    uint64_t base_ts = get_timestamp_us();
    
    // Add only 2 seconds of frames into a 30s buffer
    for (int i = 0; i < 60; ++i) {
        auto frame = create_test_h264_frame(base_ts + i * 33333, i % 30 == 0);
        extractor.add_frame(std::move(frame));
    }
    
    // Request 10 seconds — within capacity but buffer only has ~2s
    uint64_t id = extractor.request_clip(-10, 0);
    std::string path = extractor.wait_for_clip(id, 5000);
    
    extractor.stop();
    
    // Clip should succeed with whatever data is available
    ASSERT_FALSE(path.empty()) << "Still-filling request should succeed with available data";
    EXPECT_TRUE(std::filesystem::exists(path));
    
    auto stats = extractor.get_stats();
    EXPECT_EQ(stats.clips_completed, 1);
}