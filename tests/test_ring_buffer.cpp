#include <gtest/gtest.h>
#include "camera_daemon/ring_buffer.hpp"
#include <thread>
#include <chrono>

using namespace camera_daemon;

class RingBufferTest : public ::testing::Test {
protected:
    void SetUp() override {
        // 5 seconds buffer at 30fps
        buffer = std::make_unique<EncodedRingBuffer>(5, 30);
    }

    EncodedFrame make_frame(uint64_t seq, uint64_t timestamp_us, bool keyframe = false, size_t size = 1000) {
        EncodedFrame frame;
        frame.metadata.sequence = seq;
        frame.metadata.timestamp_us = timestamp_us;
        frame.metadata.is_keyframe = keyframe;
        frame.metadata.width = 1280;
        frame.metadata.height = 720;
        frame.data.resize(size, 0x42);
        return frame;
    }

    std::unique_ptr<EncodedRingBuffer> buffer;
};

TEST_F(RingBufferTest, EmptyBufferStats) {
    auto stats = buffer->get_stats();
    EXPECT_EQ(stats.frame_count, 0);
    EXPECT_EQ(stats.total_bytes, 0);
}

TEST_F(RingBufferTest, PushSingleFrame) {
    buffer->push(make_frame(1, 1000000, true));
    
    auto stats = buffer->get_stats();
    EXPECT_EQ(stats.frame_count, 1);
    EXPECT_EQ(stats.total_bytes, 1000);
}

TEST_F(RingBufferTest, PushMultipleFrames) {
    // Push 30 frames (1 second at 30fps)
    uint64_t timestamp = 0;
    for (int i = 0; i < 30; i++) {
        buffer->push(make_frame(i, timestamp, i == 0));
        timestamp += 33333;  // ~30fps
    }
    
    auto stats = buffer->get_stats();
    EXPECT_EQ(stats.frame_count, 30);
    EXPECT_EQ(stats.total_bytes, 30000);
}

TEST_F(RingBufferTest, EvictsOldFrames) {
    // Push 10 seconds worth of frames (should evict to keep only 5 seconds)
    uint64_t timestamp = 0;
    for (int i = 0; i < 300; i++) {  // 10 seconds at 30fps
        buffer->push(make_frame(i, timestamp, i % 30 == 0));  // Keyframe every second
        timestamp += 33333;
    }
    
    auto stats = buffer->get_stats();
    // Should have approximately 5 seconds of frames (150 frames)
    EXPECT_LE(stats.frame_count, 180);  // Allow some margin
    EXPECT_GE(stats.frame_count, 120);
}

TEST_F(RingBufferTest, ExtractLastSecondsEmpty) {
    auto frames = buffer->extract_last_seconds(5);
    EXPECT_TRUE(frames.empty());
}

TEST_F(RingBufferTest, ExtractLastSecondsWithData) {
    // Push 3 seconds of frames
    uint64_t timestamp = 0;
    for (int i = 0; i < 90; i++) {  // 3 seconds at 30fps
        buffer->push(make_frame(i, timestamp, i % 30 == 0));
        timestamp += 33333;
    }
    
    // Extract last 2 seconds
    auto frames = buffer->extract_last_seconds(2);
    
    // Should start from a keyframe and contain ~60 frames
    ASSERT_FALSE(frames.empty());
    EXPECT_TRUE(frames[0].metadata.is_keyframe);
    EXPECT_GE(frames.size(), 50);
    EXPECT_LE(frames.size(), 90);
}

TEST_F(RingBufferTest, Clear) {
    buffer->push(make_frame(1, 1000000, true));
    buffer->push(make_frame(2, 1033333, false));
    
    buffer->clear();
    
    auto stats = buffer->get_stats();
    EXPECT_EQ(stats.frame_count, 0);
    EXPECT_EQ(stats.total_bytes, 0);
}

TEST_F(RingBufferTest, ThreadSafety) {
    // Test concurrent pushes
    std::atomic<int> pushed{0};
    
    auto pusher = [this, &pushed]() {
        for (int i = 0; i < 100; i++) {
            buffer->push(make_frame(pushed++, pushed * 33333, i % 30 == 0));
        }
    };
    
    std::thread t1(pusher);
    std::thread t2(pusher);
    
    t1.join();
    t2.join();
    
    auto stats = buffer->get_stats();
    EXPECT_GT(stats.frame_count, 0);
}
