#include <gtest/gtest.h>
#include <cstring>
#include <thread>
#include <chrono>
#include <regex>
#include "camera_daemon/common.hpp"

using namespace camera_daemon;

// Test FrameMetadata defaults
TEST(FrameMetadataTest, DefaultValues) {
    FrameMetadata meta{};
    EXPECT_EQ(meta.timestamp_us, 0);
    EXPECT_EQ(meta.sequence, 0);
    EXPECT_EQ(meta.width, 0);
    EXPECT_EQ(meta.height, 0);
    EXPECT_FALSE(meta.is_keyframe);
    EXPECT_EQ(meta.dmabuf_fd, -1);
}

TEST(FrameMetadataTest, SetValues) {
    FrameMetadata meta;
    meta.timestamp_us = 1000000;
    meta.sequence = 42;
    meta.width = 1920;
    meta.height = 1080;
    meta.stride = 1920;
    meta.size = 1920 * 1080 * 3 / 2;
    meta.is_keyframe = true;
    meta.format = PIXFMT_YUV420;
    meta.dmabuf_fd = 7;
    
    EXPECT_EQ(meta.timestamp_us, 1000000);
    EXPECT_EQ(meta.sequence, 42);
    EXPECT_EQ(meta.width, 1920);
    EXPECT_EQ(meta.height, 1080);
    EXPECT_TRUE(meta.is_keyframe);
    EXPECT_EQ(meta.format, PIXFMT_YUV420);
    EXPECT_EQ(meta.dmabuf_fd, 7);
}

// Test EncodedFrame
TEST(EncodedFrameTest, Construction) {
    EncodedFrame frame;
    frame.metadata.width = 1280;
    frame.metadata.height = 720;
    frame.metadata.is_keyframe = true;
    frame.data = {0x00, 0x00, 0x00, 0x01, 0x67};  // H.264 SPS NAL
    
    EXPECT_EQ(frame.metadata.width, 1280);
    EXPECT_EQ(frame.data.size(), 5);
    EXPECT_EQ(frame.data[3], 0x01);
}

// Test CameraConfig defaults
TEST(CameraConfigTest, DefaultValues) {
    CameraConfig config;
    EXPECT_EQ(config.width, 1280);
    EXPECT_EQ(config.height, 960);
    EXPECT_EQ(config.framerate, 30);
    EXPECT_EQ(config.bitrate, 4000000);
    EXPECT_EQ(config.keyframe_interval, 30);
    EXPECT_EQ(config.jpeg_quality, 90);
}

// Test DaemonConfig defaults
TEST(DaemonConfigTest, DefaultValues) {
    DaemonConfig config;
    EXPECT_EQ(config.socket_path, DEFAULT_SOCKET_PATH);
    EXPECT_EQ(config.stills_dir, DEFAULT_STILLS_DIR);
    EXPECT_EQ(config.clips_dir, DEFAULT_CLIPS_DIR);
    EXPECT_EQ(config.ring_buffer_seconds, 30);
    EXPECT_EQ(config.raw_buffer_seconds, 5);
    EXPECT_EQ(config.post_event_seconds, 10);
    EXPECT_TRUE(config.enable_rtsp);
    EXPECT_EQ(config.rtsp_port, 8554);
}

// Test pixel format constants
TEST(PixelFormatTest, Constants) {
    EXPECT_EQ(PIXFMT_YUV420, 1);
    EXPECT_EQ(PIXFMT_BGR24, 4);
}

// Test VERSION constant
TEST(VersionTest, Defined) {
    EXPECT_NE(VERSION, nullptr);
    EXPECT_GT(strlen(VERSION), 0);
}

// Test get_timestamp_us()
TEST(TimestampTest, GetTimestampUsReturnsReasonableValue) {
    uint64_t ts = get_timestamp_us();
    
    // Should be after Jan 1, 2020 (in microseconds)
    uint64_t jan_2020_us = 1577836800ULL * 1000000ULL;
    EXPECT_GT(ts, jan_2020_us);
    
    // Should be before year 2100 (sanity check)
    uint64_t jan_2100_us = 4102444800ULL * 1000000ULL;
    EXPECT_LT(ts, jan_2100_us);
}

TEST(TimestampTest, GetTimestampUsIncreases) {
    uint64_t ts1 = get_timestamp_us();
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    uint64_t ts2 = get_timestamp_us();
    
    EXPECT_GT(ts2, ts1);
    // At least 4ms should have passed (allow for timing variance)
    EXPECT_GE(ts2 - ts1, 4000);
}

TEST(TimestampTest, GetTimestampUsConsistent) {
    // Multiple calls in sequence should be monotonically increasing
    uint64_t prev = get_timestamp_us();
    for (int i = 0; i < 100; ++i) {
        uint64_t curr = get_timestamp_us();
        EXPECT_GE(curr, prev);
        prev = curr;
    }
}

// Test timestamp_to_filename()
TEST(TimestampTest, FilenameFormat) {
    std::string filename = timestamp_to_filename();
    
    // Should match format: YYYYMMDD_HHMMSS_mmm
    // e.g., "20240115_143052_123"
    std::regex pattern(R"(\d{8}_\d{6}_\d{1,3})");
    EXPECT_TRUE(std::regex_match(filename, pattern)) 
        << "Filename '" << filename << "' doesn't match expected format";
}

TEST(TimestampTest, FilenameLength) {
    std::string filename = timestamp_to_filename();
    
    // Min length: 20 (YYYYMMDD_HHMMSS_0)
    // Max length: 22 (YYYYMMDD_HHMMSS_999)
    EXPECT_GE(filename.length(), 17);
    EXPECT_LE(filename.length(), 22);
}

TEST(TimestampTest, FilenameUniqueness) {
    // Different calls should produce different results (usually)
    std::string f1 = timestamp_to_filename();
    std::this_thread::sleep_for(std::chrono::milliseconds(2));
    std::string f2 = timestamp_to_filename();
    
    // With 1ms+ delay, milliseconds should differ
    EXPECT_NE(f1, f2);
}

TEST(TimestampTest, FilenameContainsDate) {
    std::string filename = timestamp_to_filename();
    
    // First 8 chars should be valid date (20XX)
    std::string year = filename.substr(0, 4);
    int year_val = std::stoi(year);
    EXPECT_GE(year_val, 2020);
    EXPECT_LE(year_val, 2100);
}

TEST(TimestampTest, FilenameContainsTime) {
    std::string filename = timestamp_to_filename();
    
    // Chars 9-14 (after underscore) should be valid time (HHMMSS)
    std::string hour = filename.substr(9, 2);
    int hour_val = std::stoi(hour);
    EXPECT_GE(hour_val, 0);
    EXPECT_LE(hour_val, 23);
    
    std::string min = filename.substr(11, 2);
    int min_val = std::stoi(min);
    EXPECT_GE(min_val, 0);
    EXPECT_LE(min_val, 59);
    
    std::string sec = filename.substr(13, 2);
    int sec_val = std::stoi(sec);
    EXPECT_GE(sec_val, 0);
    EXPECT_LE(sec_val, 59);
}
