#include <gtest/gtest.h>
#include "camera_daemon/still_capture.hpp"
#include "camera_daemon/clip_extractor.hpp"
#include "camera_daemon/v4l2_encoder.hpp"
#include "camera_daemon/v4l2_jpeg.hpp"

using namespace camera_daemon;

// ========== StillCapture Tests ==========

TEST(StillCaptureConfigTest, DefaultValues) {
    StillCapture::Config config;
    
    EXPECT_EQ(config.output_dir, DEFAULT_STILLS_DIR);
    EXPECT_EQ(config.jpeg_quality, 90);
    EXPECT_TRUE(config.embed_timestamp);
    EXPECT_EQ(config.width, 1280);
    EXPECT_EQ(config.height, 960);
}

TEST(StillCaptureConfigTest, CustomValues) {
    StillCapture::Config config;
    config.output_dir = "/custom/stills";
    config.jpeg_quality = 75;
    config.embed_timestamp = false;
    config.width = 1920;
    config.height = 1080;
    
    EXPECT_EQ(config.output_dir, "/custom/stills");
    EXPECT_EQ(config.jpeg_quality, 75);
    EXPECT_FALSE(config.embed_timestamp);
    EXPECT_EQ(config.width, 1920);
    EXPECT_EQ(config.height, 1080);
}

TEST(StillCaptureStatsTest, DefaultValues) {
    StillCapture::Stats stats{};
    
    EXPECT_EQ(stats.captures_requested, 0);
    EXPECT_EQ(stats.captures_completed, 0);
    EXPECT_EQ(stats.captures_failed, 0);
    EXPECT_EQ(stats.average_encode_time_us, 0);
}

TEST(StillCaptureStatsTest, SetValues) {
    StillCapture::Stats stats;
    stats.captures_requested = 100;
    stats.captures_completed = 95;
    stats.captures_failed = 5;
    stats.average_encode_time_us = 25000;
    
    EXPECT_EQ(stats.captures_requested, 100);
    EXPECT_EQ(stats.captures_completed, 95);
    EXPECT_EQ(stats.captures_failed, 5);
    EXPECT_EQ(stats.average_encode_time_us, 25000);
}

// ========== ClipExtractor Tests ==========

TEST(ClipExtractorConfigTest, DefaultValues) {
    ClipExtractor::Config config;
    
    EXPECT_EQ(config.output_dir, DEFAULT_CLIPS_DIR);
    EXPECT_EQ(config.pre_event_seconds, 10);
    EXPECT_EQ(config.post_event_seconds, 5);
    EXPECT_TRUE(config.remux_to_mp4);
}

TEST(ClipExtractorConfigTest, CustomValues) {
    ClipExtractor::Config config;
    config.output_dir = "/custom/clips";
    config.pre_event_seconds = 30;
    config.post_event_seconds = 15;
    config.remux_to_mp4 = false;
    
    EXPECT_EQ(config.output_dir, "/custom/clips");
    EXPECT_EQ(config.pre_event_seconds, 30);
    EXPECT_EQ(config.post_event_seconds, 15);
    EXPECT_FALSE(config.remux_to_mp4);
}

TEST(ClipExtractorStatsTest, DefaultValues) {
    ClipExtractor::Stats stats{};
    
    EXPECT_EQ(stats.clips_requested, 0);
    EXPECT_EQ(stats.clips_completed, 0);
    EXPECT_EQ(stats.clips_failed, 0);
    EXPECT_EQ(stats.total_bytes_written, 0);
}

TEST(ClipExtractorStatsTest, SetValues) {
    ClipExtractor::Stats stats;
    stats.clips_requested = 50;
    stats.clips_completed = 48;
    stats.clips_failed = 2;
    stats.total_bytes_written = 1024 * 1024 * 100;  // 100 MB
    
    EXPECT_EQ(stats.clips_requested, 50);
    EXPECT_EQ(stats.clips_completed, 48);
    EXPECT_EQ(stats.clips_failed, 2);
    EXPECT_EQ(stats.total_bytes_written, 104857600);
}

TEST(ClipRequestTest, DefaultValues) {
    ClipExtractor::ClipRequest request{};
    
    EXPECT_EQ(request.request_id, 0);
    EXPECT_EQ(request.start_offset, 0);
    EXPECT_EQ(request.end_offset, 0);
    EXPECT_EQ(request.request_timestamp, 0);
}

TEST(ClipRequestTest, SetValues) {
    ClipExtractor::ClipRequest request;
    request.request_id = 42;
    request.start_offset = -5;  // 5 seconds ago
    request.end_offset = 5;     // 5 seconds from now
    request.request_timestamp = 1700000000000000ULL;
    
    EXPECT_EQ(request.request_id, 42);
    EXPECT_EQ(request.start_offset, -5);
    EXPECT_EQ(request.end_offset, 5);
    EXPECT_EQ(request.request_timestamp, 1700000000000000ULL);
}

// ========== V4L2Encoder Tests ==========

TEST(V4L2EncoderConfigTest, DefaultCodec) {
    V4L2Encoder::Config config{};
    
    EXPECT_EQ(config.codec, V4L2Encoder::Codec::H264);
}

TEST(V4L2EncoderConfigTest, SetValues) {
    V4L2Encoder::Config config;
    config.width = 1920;
    config.height = 1080;
    config.framerate = 30;
    config.bitrate = 4000000;
    config.keyframe_interval = 30;
    config.codec = V4L2Encoder::Codec::H265;
    
    EXPECT_EQ(config.width, 1920);
    EXPECT_EQ(config.height, 1080);
    EXPECT_EQ(config.framerate, 30);
    EXPECT_EQ(config.bitrate, 4000000);
    EXPECT_EQ(config.keyframe_interval, 30);
    EXPECT_EQ(config.codec, V4L2Encoder::Codec::H265);
}

TEST(V4L2EncoderCodecTest, EnumValues) {
    EXPECT_NE(V4L2Encoder::Codec::H264, V4L2Encoder::Codec::H265);
}

TEST(V4L2EncoderStatsTest, DefaultValues) {
    V4L2Encoder::Stats stats{};
    
    EXPECT_EQ(stats.frames_in, 0);
    EXPECT_EQ(stats.frames_out, 0);
    EXPECT_EQ(stats.bytes_out, 0);
    EXPECT_EQ(stats.dropped_frames, 0);
}

TEST(V4L2EncoderStatsTest, SetValues) {
    V4L2Encoder::Stats stats;
    stats.frames_in = 1000;
    stats.frames_out = 999;
    stats.bytes_out = 50000000;
    stats.dropped_frames = 1;
    
    EXPECT_EQ(stats.frames_in, 1000);
    EXPECT_EQ(stats.frames_out, 999);
    EXPECT_EQ(stats.bytes_out, 50000000);
    EXPECT_EQ(stats.dropped_frames, 1);
}

// ========== V4L2JpegEncoder Tests ==========

TEST(V4L2JpegConfigTest, DefaultValues) {
    V4L2JpegEncoder::Config config;
    
    EXPECT_EQ(config.width, 1280);
    EXPECT_EQ(config.height, 960);
    EXPECT_EQ(config.quality, 90);
}

TEST(V4L2JpegConfigTest, QualityRange) {
    V4L2JpegEncoder::Config config;
    
    // Test boundary values
    config.quality = 1;
    EXPECT_EQ(config.quality, 1);
    
    config.quality = 100;
    EXPECT_EQ(config.quality, 100);
}

TEST(V4L2JpegConfigTest, CustomValues) {
    V4L2JpegEncoder::Config config;
    config.width = 4096;
    config.height = 2160;
    config.quality = 75;
    
    EXPECT_EQ(config.width, 4096);
    EXPECT_EQ(config.height, 2160);
    EXPECT_EQ(config.quality, 75);
}
