#include <gtest/gtest.h>
#include "camera_daemon/v4l2_jpeg.hpp"
#include "camera_daemon/v4l2_encoder.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <cstring>

using namespace camera_daemon;

// ========== Hardware JPEG Encoder Tests ==========

class V4L2JpegEncoderTest : public ::testing::Test {
protected:
    bool has_hardware = false;
    
    void SetUp() override {
        has_hardware = V4L2JpegEncoder::is_available();
    }
};

TEST_F(V4L2JpegEncoderTest, IsAvailableReturnsConsistentResult) {
    // Multiple calls should return the same result
    bool result1 = V4L2JpegEncoder::is_available();
    bool result2 = V4L2JpegEncoder::is_available();
    EXPECT_EQ(result1, result2);
}

TEST_F(V4L2JpegEncoderTest, InitializeWithValidConfig) {
    if (!has_hardware) {
        GTEST_SKIP() << "Hardware JPEG encoder not available";
    }
    
    V4L2JpegEncoder encoder;
    V4L2JpegEncoder::Config config;
    config.width = 1280;
    config.height = 960;
    config.quality = 90;
    
    EXPECT_TRUE(encoder.initialize(config));
}

TEST_F(V4L2JpegEncoderTest, InitializeWithDifferentResolutions) {
    if (!has_hardware) {
        GTEST_SKIP() << "Hardware JPEG encoder not available";
    }
    
    // Test common resolutions
    std::vector<std::pair<uint32_t, uint32_t>> resolutions = {
        {640, 480},
        {1280, 720},
        {1280, 960},
        {1920, 1080},
    };
    
    for (const auto& [w, h] : resolutions) {
        V4L2JpegEncoder encoder;
        V4L2JpegEncoder::Config config;
        config.width = w;
        config.height = h;
        config.quality = 85;
        
        EXPECT_TRUE(encoder.initialize(config)) 
            << "Failed to init at " << w << "x" << h;
    }
}

TEST_F(V4L2JpegEncoderTest, EncodeValidYUVData) {
    if (!has_hardware) {
        GTEST_SKIP() << "Hardware JPEG encoder not available";
    }
    
    V4L2JpegEncoder encoder;
    V4L2JpegEncoder::Config config;
    config.width = 640;
    config.height = 480;
    config.quality = 80;
    
    ASSERT_TRUE(encoder.initialize(config));
    
    // Create test YUV420 data (gray frame)
    size_t yuv_size = 640 * 480 * 3 / 2;
    std::vector<uint8_t> yuv_data(yuv_size);
    
    // Y plane = 128 (mid-gray)
    memset(yuv_data.data(), 128, 640 * 480);
    // U and V planes = 128 (neutral chroma)
    memset(yuv_data.data() + 640 * 480, 128, 640 * 480 / 2);
    
    std::vector<uint8_t> jpeg_out;
    EXPECT_TRUE(encoder.encode(yuv_data.data(), yuv_size, jpeg_out));
    
    // JPEG should be smaller than raw YUV
    EXPECT_GT(jpeg_out.size(), 0);
    EXPECT_LT(jpeg_out.size(), yuv_size);
    
    // Check JPEG magic bytes (SOI marker)
    ASSERT_GE(jpeg_out.size(), 2);
    EXPECT_EQ(jpeg_out[0], 0xFF);
    EXPECT_EQ(jpeg_out[1], 0xD8);
}

TEST_F(V4L2JpegEncoderTest, EncodeMultipleFrames) {
    if (!has_hardware) {
        GTEST_SKIP() << "Hardware JPEG encoder not available";
    }
    
    V4L2JpegEncoder encoder;
    V4L2JpegEncoder::Config config;
    config.width = 640;
    config.height = 480;
    config.quality = 85;
    
    ASSERT_TRUE(encoder.initialize(config));
    
    size_t yuv_size = 640 * 480 * 3 / 2;
    std::vector<uint8_t> yuv_data(yuv_size, 128);
    
    // Encode 10 frames
    for (int i = 0; i < 10; ++i) {
        std::vector<uint8_t> jpeg_out;
        EXPECT_TRUE(encoder.encode(yuv_data.data(), yuv_size, jpeg_out))
            << "Failed on frame " << i;
        EXPECT_GT(jpeg_out.size(), 0);
    }
}

TEST_F(V4L2JpegEncoderTest, QualityAffectsSize) {
    if (!has_hardware) {
        GTEST_SKIP() << "Hardware JPEG encoder not available";
    }
    
    size_t yuv_size = 640 * 480 * 3 / 2;
    std::vector<uint8_t> yuv_data(yuv_size);
    
    // Create a complex pattern (noise-like) that will compress differently
    // at different quality levels
    for (size_t i = 0; i < 640 * 480; ++i) {
        // Pseudo-random pattern based on position
        yuv_data[i] = static_cast<uint8_t>((i * 7 + (i / 640) * 13 + (i % 640) * 17) % 256);
    }
    // Chroma with variation too
    for (size_t i = 640 * 480; i < yuv_size; ++i) {
        yuv_data[i] = static_cast<uint8_t>((i * 11 + 37) % 256);
    }
    
    size_t size_q50, size_q95;
    
    // Low quality
    {
        V4L2JpegEncoder encoder;
        V4L2JpegEncoder::Config config{640, 480, 50};
        ASSERT_TRUE(encoder.initialize(config));
        
        std::vector<uint8_t> jpeg_out;
        ASSERT_TRUE(encoder.encode(yuv_data.data(), yuv_size, jpeg_out));
        size_q50 = jpeg_out.size();
    }
    
    // High quality
    {
        V4L2JpegEncoder encoder;
        V4L2JpegEncoder::Config config{640, 480, 95};
        ASSERT_TRUE(encoder.initialize(config));
        
        std::vector<uint8_t> jpeg_out;
        ASSERT_TRUE(encoder.encode(yuv_data.data(), yuv_size, jpeg_out));
        size_q95 = jpeg_out.size();
    }
    
    // Higher quality should produce larger file for complex images
    // If they're equal, at least neither should be zero
    EXPECT_GT(size_q50, 0);
    EXPECT_GT(size_q95, 0);
    // For complex patterns, high quality should be larger
    EXPECT_GE(size_q95, size_q50);
}

// ========== Hardware H.264 Encoder Tests ==========

class V4L2EncoderTest : public ::testing::Test {
protected:
    bool has_hardware = false;
    
    void SetUp() override {
        int fd = open("/dev/video11", O_RDWR);
        if (fd >= 0) {
            has_hardware = true;
            close(fd);
        }
    }
};

TEST_F(V4L2EncoderTest, DeviceExists) {
    // Just check we can detect hardware
    if (has_hardware) {
        SUCCEED() << "H.264 encoder available at /dev/video11";
    } else {
        GTEST_SKIP() << "H.264 encoder not available";
    }
}

TEST_F(V4L2EncoderTest, InitializeWithValidConfig) {
    if (!has_hardware) {
        GTEST_SKIP() << "H.264 encoder not available";
    }
    
    V4L2Encoder encoder;
    V4L2Encoder::Config config;
    config.width = 1280;
    config.height = 960;
    config.framerate = 30;
    config.bitrate = 4000000;
    config.keyframe_interval = 30;
    config.codec = V4L2Encoder::Codec::H264;
    
    EXPECT_TRUE(encoder.initialize(config));
}

TEST_F(V4L2EncoderTest, InitializeWithDifferentBitrates) {
    if (!has_hardware) {
        GTEST_SKIP() << "H.264 encoder not available";
    }
    
    std::vector<uint32_t> bitrates = {1000000, 2000000, 4000000, 8000000};
    
    for (uint32_t bitrate : bitrates) {
        V4L2Encoder encoder;
        V4L2Encoder::Config config;
        config.width = 1280;
        config.height = 720;
        config.framerate = 30;
        config.bitrate = bitrate;
        config.keyframe_interval = 30;
        
        EXPECT_TRUE(encoder.initialize(config))
            << "Failed to init at bitrate " << bitrate;
    }
}

TEST_F(V4L2EncoderTest, StartStop) {
    if (!has_hardware) {
        GTEST_SKIP() << "H.264 encoder not available";
    }
    
    V4L2Encoder encoder;
    V4L2Encoder::Config config;
    config.width = 1280;
    config.height = 720;
    config.framerate = 30;
    config.bitrate = 4000000;
    config.keyframe_interval = 30;
    
    ASSERT_TRUE(encoder.initialize(config));
    
    EXPECT_TRUE(encoder.start());
    EXPECT_TRUE(encoder.is_running());
    
    encoder.stop();
    EXPECT_FALSE(encoder.is_running());
}

TEST_F(V4L2EncoderTest, DoubleStart) {
    if (!has_hardware) {
        GTEST_SKIP() << "H.264 encoder not available";
    }
    
    V4L2Encoder encoder;
    V4L2Encoder::Config config;
    config.width = 1280;
    config.height = 720;
    config.framerate = 30;
    config.bitrate = 4000000;
    config.keyframe_interval = 30;
    
    ASSERT_TRUE(encoder.initialize(config));
    
    EXPECT_TRUE(encoder.start());
    EXPECT_TRUE(encoder.start());  // Second start should be OK
    
    encoder.stop();
}

TEST_F(V4L2EncoderTest, StatsInitiallyZero) {
    if (!has_hardware) {
        GTEST_SKIP() << "H.264 encoder not available";
    }
    
    V4L2Encoder encoder;
    V4L2Encoder::Config config;
    config.width = 1280;
    config.height = 720;
    config.framerate = 30;
    config.bitrate = 4000000;
    config.keyframe_interval = 30;
    
    ASSERT_TRUE(encoder.initialize(config));
    
    auto stats = encoder.get_stats();
    EXPECT_EQ(stats.frames_in, 0);
    EXPECT_EQ(stats.frames_out, 0);
    EXPECT_EQ(stats.bytes_out, 0);
    EXPECT_EQ(stats.dropped_frames, 0);
}

TEST_F(V4L2EncoderTest, ForceKeyframe) {
    if (!has_hardware) {
        GTEST_SKIP() << "H.264 encoder not available";
    }
    
    V4L2Encoder encoder;
    V4L2Encoder::Config config;
    config.width = 1280;
    config.height = 720;
    config.framerate = 30;
    config.bitrate = 4000000;
    config.keyframe_interval = 30;
    
    ASSERT_TRUE(encoder.initialize(config));
    ASSERT_TRUE(encoder.start());
    
    // Should not throw or crash
    encoder.force_keyframe();
    
    encoder.stop();
}

TEST_F(V4L2EncoderTest, SetOutputCallback) {
    if (!has_hardware) {
        GTEST_SKIP() << "H.264 encoder not available";
    }
    
    V4L2Encoder encoder;
    V4L2Encoder::Config config;
    config.width = 1280;
    config.height = 720;
    config.framerate = 30;
    config.bitrate = 4000000;
    config.keyframe_interval = 30;
    
    ASSERT_TRUE(encoder.initialize(config));
    
    int callback_count = 0;
    encoder.set_output_callback([&](const EncodedFrame& frame) {
        callback_count++;
    });
    
    // Callback set but not called yet (no frames encoded)
    EXPECT_EQ(callback_count, 0);
}

// Test that encoder cleans up properly on destruction
TEST_F(V4L2EncoderTest, DestructorCleansUp) {
    if (!has_hardware) {
        GTEST_SKIP() << "H.264 encoder not available";
    }
    
    {
        V4L2Encoder encoder;
        V4L2Encoder::Config config;
        config.width = 1280;
        config.height = 720;
        config.framerate = 30;
        config.bitrate = 4000000;
        config.keyframe_interval = 30;
        
        encoder.initialize(config);
        encoder.start();
        // Destructor should clean up properly
    }
    
    // Should be able to create another encoder after previous one destroyed
    {
        V4L2Encoder encoder;
        V4L2Encoder::Config config;
        config.width = 1280;
        config.height = 720;
        config.framerate = 30;
        config.bitrate = 4000000;
        config.keyframe_interval = 30;
        
        EXPECT_TRUE(encoder.initialize(config));
    }
}
