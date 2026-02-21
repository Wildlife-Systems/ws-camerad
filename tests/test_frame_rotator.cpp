#include <gtest/gtest.h>
#include "camera_daemon/frame_rotator.hpp"
#include "camera_daemon/v4l2_loopback.hpp"
#include "camera_daemon/common.hpp"
#include <vector>
#include <cstring>

using namespace camera_daemon;

// ============================================================================
// FRAME ROTATOR TESTS
// ============================================================================

class FrameRotatorTest : public ::testing::Test {
protected:
    // Create a test YUV420 frame with a recognizable pattern
    // The pattern has a distinct top-left corner so rotation is verifiable
    std::vector<uint8_t> create_test_frame(uint32_t width, uint32_t height) {
        size_t y_size = width * height;
        size_t uv_size = (width / 2) * (height / 2);
        std::vector<uint8_t> frame(y_size + uv_size * 2);

        // Y plane: gradient with bright top-left corner marker
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                // Mark top-left 8x8 region as bright (255)
                if (x < 8 && y < 8) {
                    frame[y * width + x] = 255;
                }
                // Mark bottom-right 8x8 region as dark (0)
                else if (x >= width - 8 && y >= height - 8) {
                    frame[y * width + x] = 0;
                }
                // Rest is gradient
                else {
                    frame[y * width + x] = (x + y) % 256;
                }
            }
        }

        // U plane: constant 128
        std::memset(frame.data() + y_size, 128, uv_size);

        // V plane: constant 128 
        std::memset(frame.data() + y_size + uv_size, 128, uv_size);

        return frame;
    }

    // Check if a region has a specific value (for verifying marker position)
    bool region_has_value(const uint8_t* data, uint32_t width, uint32_t height,
                          uint32_t region_x, uint32_t region_y,
                          uint32_t region_size, uint8_t expected_value) {
        for (uint32_t y = region_y; y < region_y + region_size && y < height; y++) {
            for (uint32_t x = region_x; x < region_x + region_size && x < width; x++) {
                if (data[y * width + x] != expected_value) {
                    return false;
                }
            }
        }
        return true;
    }
};

TEST_F(FrameRotatorTest, DimensionsSwapFor90Degrees) {
    uint32_t src_w = 640;
    uint32_t src_h = 480;
    
    FrameRotator rotator(src_w, src_h, FrameRotator::Rotation::Rot90);
    
    // After 90° rotation, dimensions swap
    EXPECT_EQ(rotator.dst_width(), 480);
    EXPECT_EQ(rotator.dst_height(), 640);
}

TEST_F(FrameRotatorTest, DimensionsSwapFor270Degrees) {
    uint32_t src_w = 640;
    uint32_t src_h = 480;
    
    FrameRotator rotator(src_w, src_h, FrameRotator::Rotation::Rot270);
    
    // After 270° rotation, dimensions swap
    EXPECT_EQ(rotator.dst_width(), 480);
    EXPECT_EQ(rotator.dst_height(), 640);
}

TEST_F(FrameRotatorTest, RotatedFrameSizeCorrect) {
    uint32_t src_w = 640;
    uint32_t src_h = 480;
    
    FrameRotator rotator(src_w, src_h, FrameRotator::Rotation::Rot90);
    
    // YUV420: 1.5 bytes per pixel
    size_t expected_size = rotator.dst_width() * rotator.dst_height() * 3 / 2;
    EXPECT_EQ(rotator.rotated_size(), expected_size);
}

TEST_F(FrameRotatorTest, Rotate90ClockwiseMovesTopLeftToTopRight) {
    uint32_t src_w = 32;
    uint32_t src_h = 24;
    
    auto src_frame = create_test_frame(src_w, src_h);
    
    FrameRotator rotator(src_w, src_h, FrameRotator::Rotation::Rot90);
    const uint8_t* rotated = rotator.rotate(src_frame.data(), src_w);
    
    uint32_t dst_w = rotator.dst_width();   // 24
    uint32_t dst_h = rotator.dst_height();  // 32
    
    // After 90° CW rotation:
    // - Original top-left (0,0) -> top-right (w-1, 0) in rotated
    // - Original bottom-right -> bottom-left
    
    // The bright marker (255) should now be at top-right
    EXPECT_TRUE(region_has_value(rotated, dst_w, dst_h, 
                                  dst_w - 8, 0, 8, 255))
        << "Bright marker should be at top-right after 90° rotation";
    
    // The dark marker (0) should now be at bottom-left
    EXPECT_TRUE(region_has_value(rotated, dst_w, dst_h,
                                  0, dst_h - 8, 8, 0))
        << "Dark marker should be at bottom-left after 90° rotation";
}

TEST_F(FrameRotatorTest, Rotate270ClockwiseMovesTopLeftToBottomLeft) {
    uint32_t src_w = 32;
    uint32_t src_h = 24;
    
    auto src_frame = create_test_frame(src_w, src_h);
    
    FrameRotator rotator(src_w, src_h, FrameRotator::Rotation::Rot270);
    const uint8_t* rotated = rotator.rotate(src_frame.data(), src_w);
    
    uint32_t dst_w = rotator.dst_width();   // 24
    uint32_t dst_h = rotator.dst_height();  // 32
    
    // After 270° CW rotation (= 90° CCW):
    // - Original top-left -> bottom-left
    // - Original bottom-right -> top-right
    
    // The bright marker (255) should now be at bottom-left
    EXPECT_TRUE(region_has_value(rotated, dst_w, dst_h,
                                  0, dst_h - 8, 8, 255))
        << "Bright marker should be at bottom-left after 270° rotation";
    
    // The dark marker (0) should now be at top-right
    EXPECT_TRUE(region_has_value(rotated, dst_w, dst_h,
                                  dst_w - 8, 0, 8, 0))
        << "Dark marker should be at top-right after 270° rotation";
}

TEST_F(FrameRotatorTest, DoubleRotation180Preserves) {
    uint32_t src_w = 32;
    uint32_t src_h = 24;
    
    auto src_frame = create_test_frame(src_w, src_h);
    
    // Rotate 90° twice = 180°
    FrameRotator rotator1(src_w, src_h, FrameRotator::Rotation::Rot90);
    const uint8_t* rotated1 = rotator1.rotate(src_frame.data(), src_w);
    
    // Copy intermediate result (rotator1 owns the buffer)
    std::vector<uint8_t> intermediate(rotator1.rotated_size());
    std::memcpy(intermediate.data(), rotated1, intermediate.size());
    
    FrameRotator rotator2(rotator1.dst_width(), rotator1.dst_height(), 
                           FrameRotator::Rotation::Rot90);
    const uint8_t* rotated2 = rotator2.rotate(intermediate.data(), rotator1.dst_width());
    
    // After two 90° rotations, dimensions return to original
    EXPECT_EQ(rotator2.dst_width(), src_w);
    EXPECT_EQ(rotator2.dst_height(), src_h);
    
    // After 180° total:
    // - Original top-left (bright) -> bottom-right
    // - Original bottom-right (dark) -> top-left
    EXPECT_TRUE(region_has_value(rotated2, src_w, src_h,
                                  src_w - 8, src_h - 8, 8, 255))
        << "Bright marker should be at bottom-right after 180° rotation";
    
    EXPECT_TRUE(region_has_value(rotated2, src_w, src_h,
                                  0, 0, 8, 0))
        << "Dark marker should be at top-left after 180° rotation";
}

TEST_F(FrameRotatorTest, LargeFrameRotation) {
    // Test with realistic resolution
    uint32_t src_w = 1280;
    uint32_t src_h = 960;
    
    auto src_frame = create_test_frame(src_w, src_h);
    
    FrameRotator rotator(src_w, src_h, FrameRotator::Rotation::Rot90);
    const uint8_t* rotated = rotator.rotate(src_frame.data(), src_w);
    
    EXPECT_EQ(rotator.dst_width(), 960);
    EXPECT_EQ(rotator.dst_height(), 1280);
    
    // Verify marker positions
    uint32_t dst_w = rotator.dst_width();
    uint32_t dst_h = rotator.dst_height();
    
    EXPECT_TRUE(region_has_value(rotated, dst_w, dst_h,
                                  dst_w - 8, 0, 8, 255))
        << "Bright marker should be at top-right after 90° rotation";
}

// ============================================================================
// VIRTUAL CAMERA WITH ROTATION TESTS
// ============================================================================

class VirtualCameraRotationTest : public ::testing::Test {
protected:
    void SetUp() override {
        auto devices = V4L2LoopbackOutput::find_loopback_devices();
        if (!devices.empty()) {
            test_device_ = devices[0];
            // Verify the device can actually be opened exclusively
            V4L2LoopbackOutput probe;
            V4L2LoopbackOutput::Config probe_config;
            probe_config.device = test_device_;
            probe_config.width = 640;
            probe_config.height = 480;
            probe_config.framerate = 30;
            if (!probe.initialize(probe_config)) {
                test_device_.clear();  // Device busy, treat as unavailable
            } else {
                probe.close();
            }
        }
    }
    
    std::string test_device_;
    
    // Create a test frame with known pattern
    std::vector<uint8_t> create_marked_frame(uint32_t width, uint32_t height) {
        size_t y_size = width * height;
        size_t uv_size = (width / 2) * (height / 2);
        std::vector<uint8_t> frame(y_size + uv_size * 2);
        
        // Y plane with markers
        for (uint32_t y = 0; y < height; y++) {
            for (uint32_t x = 0; x < width; x++) {
                if (x < 16 && y < 16) {
                    frame[y * width + x] = 255;  // Top-left bright
                } else if (x >= width - 16 && y >= height - 16) {
                    frame[y * width + x] = 0;    // Bottom-right dark
                } else {
                    frame[y * width + x] = 128;  // Middle gray
                }
            }
        }
        
        std::memset(frame.data() + y_size, 128, uv_size * 2);
        return frame;
    }
};

TEST_F(VirtualCameraRotationTest, RotatedFrameWrittenToLoopback) {
    if (test_device_.empty()) {
        GTEST_SKIP() << "No v4l2loopback device available";
    }
    
    uint32_t src_w = 640;
    uint32_t src_h = 480;
    
    // Create source frame
    auto src_frame = create_marked_frame(src_w, src_h);
    
    // Rotate it
    FrameRotator rotator(src_w, src_h, FrameRotator::Rotation::Rot90);
    const uint8_t* rotated = rotator.rotate(src_frame.data(), src_w);
    
    // Write to virtual camera
    V4L2LoopbackOutput output;
    V4L2LoopbackOutput::Config config;
    config.device = test_device_;
    config.width = rotator.dst_width();   // Dimensions are swapped
    config.height = rotator.dst_height();
    config.framerate = 30;
    
    ASSERT_TRUE(output.initialize(config));
    
    FrameMetadata meta{};
    meta.width = rotator.dst_width();
    meta.height = rotator.dst_height();
    meta.stride = rotator.dst_stride();
    meta.size = rotator.rotated_size();
    
    // Write rotated frame
    output.write_frame(rotated, rotator.rotated_size(), meta);
    
    auto stats = output.get_stats();
    EXPECT_GE(stats.frames_written + stats.frames_dropped, 1);
    
    output.close();
}

TEST_F(VirtualCameraRotationTest, MultipleRotatedFrames) {
    if (test_device_.empty()) {
        GTEST_SKIP() << "No v4l2loopback device available";
    }
    
    uint32_t src_w = 640;
    uint32_t src_h = 480;
    
    auto src_frame = create_marked_frame(src_w, src_h);
    
    FrameRotator rotator(src_w, src_h, FrameRotator::Rotation::Rot270);
    
    V4L2LoopbackOutput output;
    V4L2LoopbackOutput::Config config;
    config.device = test_device_;
    config.width = rotator.dst_width();
    config.height = rotator.dst_height();
    
    ASSERT_TRUE(output.initialize(config));
    
    FrameMetadata meta{};
    meta.width = rotator.dst_width();
    meta.height = rotator.dst_height();
    meta.stride = rotator.dst_stride();
    meta.size = rotator.rotated_size();
    
    // Write 30 rotated frames
    for (int i = 0; i < 30; i++) {
        const uint8_t* rotated = rotator.rotate(src_frame.data(), src_w);
        output.write_frame(rotated, rotator.rotated_size(), meta);
    }
    
    auto stats = output.get_stats();
    EXPECT_EQ(stats.frames_written + stats.frames_dropped, 30);
    
    output.close();
}

// ============================================================================
// ROTATION CORRECTNESS VERIFICATION
// ============================================================================

TEST(RotationCorrectnessTest, SinglePixelRotation90) {
    // 4x2 mini-frame to verify rotation math exactly
    // Source (4 wide, 2 tall):
    //   A B C D
    //   E F G H
    //
    // After 90° CW rotation (2 wide, 4 tall):
    //   E A
    //   F B 
    //   G C
    //   H D
    
    uint32_t src_w = 4;
    uint32_t src_h = 2;
    
    // Create source with known Y values (skip U/V for this test)
    std::vector<uint8_t> src(4 * 2 * 3 / 2, 128);  // YUV420 frame
    src[0] = 'A'; src[1] = 'B'; src[2] = 'C'; src[3] = 'D';
    src[4] = 'E'; src[5] = 'F'; src[6] = 'G'; src[7] = 'H';
    
    FrameRotator rotator(src_w, src_h, FrameRotator::Rotation::Rot90);
    const uint8_t* dst = rotator.rotate(src.data(), src_w);
    
    EXPECT_EQ(rotator.dst_width(), 2);
    EXPECT_EQ(rotator.dst_height(), 4);
    
    // Verify Y plane values
    // Row 0: E A
    EXPECT_EQ(dst[0], 'E');
    EXPECT_EQ(dst[1], 'A');
    // Row 1: F B
    EXPECT_EQ(dst[2], 'F');
    EXPECT_EQ(dst[3], 'B');
    // Row 2: G C
    EXPECT_EQ(dst[4], 'G');
    EXPECT_EQ(dst[5], 'C');
    // Row 3: H D
    EXPECT_EQ(dst[6], 'H');
    EXPECT_EQ(dst[7], 'D');
}

TEST(RotationCorrectnessTest, SinglePixelRotation270) {
    // 4x2 mini-frame
    // Source:
    //   A B C D
    //   E F G H
    //
    // After 270° CW rotation (= 90° CCW) (2 wide, 4 tall):
    //   D H
    //   C G
    //   B F
    //   A E
    
    uint32_t src_w = 4;
    uint32_t src_h = 2;
    
    std::vector<uint8_t> src(4 * 2 * 3 / 2, 128);
    src[0] = 'A'; src[1] = 'B'; src[2] = 'C'; src[3] = 'D';
    src[4] = 'E'; src[5] = 'F'; src[6] = 'G'; src[7] = 'H';
    
    FrameRotator rotator(src_w, src_h, FrameRotator::Rotation::Rot270);
    const uint8_t* dst = rotator.rotate(src.data(), src_w);
    
    EXPECT_EQ(rotator.dst_width(), 2);
    EXPECT_EQ(rotator.dst_height(), 4);
    
    // Row 0: D H
    EXPECT_EQ(dst[0], 'D');
    EXPECT_EQ(dst[1], 'H');
    // Row 1: C G
    EXPECT_EQ(dst[2], 'C');
    EXPECT_EQ(dst[3], 'G');
    // Row 2: B F
    EXPECT_EQ(dst[4], 'B');
    EXPECT_EQ(dst[5], 'F');
    // Row 3: A E
    EXPECT_EQ(dst[6], 'A');
    EXPECT_EQ(dst[7], 'E');
}
