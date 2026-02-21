#include <gtest/gtest.h>
#include "camera_daemon/raw_ring_buffer.hpp"
#include <thread>
#include <cstring>

using namespace camera_daemon;

class RawRingBufferTest : public ::testing::Test {
protected:
    static constexpr uint32_t WIDTH = 320;
    static constexpr uint32_t HEIGHT = 240;
    static constexpr size_t FRAME_SIZE = WIDTH * HEIGHT * 3 / 2;  // YUV420

    FrameMetadata make_meta(uint64_t seq, uint64_t timestamp_us) {
        FrameMetadata m{};
        m.sequence = seq;
        m.timestamp_us = timestamp_us;
        m.width = WIDTH;
        m.height = HEIGHT;
        m.stride = WIDTH;
        m.size = FRAME_SIZE;
        m.format = 1;  // YUV420
        return m;
    }

    std::vector<uint8_t> make_frame(uint8_t fill) {
        std::vector<uint8_t> buf(FRAME_SIZE, fill);
        return buf;
    }
};

TEST_F(RawRingBufferTest, EmptyBuffer) {
    RawRingBuffer buf(2, 30, FRAME_SIZE);  // 2 seconds @ 30fps
    EXPECT_EQ(buf.size(), 0);
    EXPECT_EQ(buf.capacity(), 60);

    auto [oldest, newest] = buf.time_range();
    EXPECT_EQ(oldest, 0);
    EXPECT_EQ(newest, 0);
}

TEST_F(RawRingBufferTest, PushAndRetrieve) {
    RawRingBuffer buf(1, 10, FRAME_SIZE);  // 1 second @ 10fps = 10 slots

    auto frame = make_frame(0xAB);
    auto meta = make_meta(1, 1000000);  // 1 second
    buf.push(frame.data(), frame.size(), meta);

    EXPECT_EQ(buf.size(), 1);

    std::vector<uint8_t> out;
    FrameMetadata out_meta;
    EXPECT_TRUE(buf.copy_nearest(1000000, out, out_meta));
    EXPECT_EQ(out.size(), FRAME_SIZE);
    EXPECT_EQ(out[0], 0xAB);
    EXPECT_EQ(out_meta.sequence, 1);
    EXPECT_EQ(out_meta.timestamp_us, 1000000);
}

TEST_F(RawRingBufferTest, FindNearest) {
    RawRingBuffer buf(5, 10, FRAME_SIZE);  // 5 sec @ 10fps = 50 slots

    // Push frames at 100ms intervals (10fps)
    for (int i = 0; i < 30; ++i) {
        auto frame = make_frame(static_cast<uint8_t>(i));
        auto meta = make_meta(i, 1000000 + i * 100000);
        buf.push(frame.data(), frame.size(), meta);
    }

    EXPECT_EQ(buf.size(), 30);

    // Request timestamp closest to frame 15 (at 2500000 us)
    std::vector<uint8_t> out;
    FrameMetadata out_meta;
    EXPECT_TRUE(buf.copy_nearest(2500000, out, out_meta));
    EXPECT_EQ(out_meta.sequence, 15);
    EXPECT_EQ(out[0], 15);
}

TEST_F(RawRingBufferTest, FindNearestBetweenFrames) {
    RawRingBuffer buf(5, 10, FRAME_SIZE);

    // Frame 0 at 1.0s, frame 1 at 1.1s
    auto f0 = make_frame(0xAA);
    buf.push(f0.data(), f0.size(), make_meta(0, 1000000));
    auto f1 = make_frame(0xBB);
    buf.push(f1.data(), f1.size(), make_meta(1, 1100000));

    // Target 1.08s — closer to frame 1
    std::vector<uint8_t> out;
    FrameMetadata out_meta;
    EXPECT_TRUE(buf.copy_nearest(1080000, out, out_meta));
    EXPECT_EQ(out_meta.sequence, 1);
    EXPECT_EQ(out[0], 0xBB);

    // Target 1.02s — closer to frame 0
    EXPECT_TRUE(buf.copy_nearest(1020000, out, out_meta));
    EXPECT_EQ(out_meta.sequence, 0);
    EXPECT_EQ(out[0], 0xAA);
}

TEST_F(RawRingBufferTest, OverwritesOldestWhenFull) {
    RawRingBuffer buf(1, 5, FRAME_SIZE);  // 1 second @ 5fps = 5 slots

    // Push 8 frames into 5 slots
    for (int i = 0; i < 8; ++i) {
        auto frame = make_frame(static_cast<uint8_t>(i));
        auto meta = make_meta(i, 1000000 + i * 200000);
        buf.push(frame.data(), frame.size(), meta);
    }

    // Only 5 should be retained
    EXPECT_EQ(buf.size(), 5);

    // The oldest should be frame 3 (frames 0-2 overwritten)
    auto [oldest, newest] = buf.time_range();
    EXPECT_EQ(oldest, 1000000 + 3 * 200000);  // frame 3
    EXPECT_EQ(newest, 1000000 + 7 * 200000);  // frame 7

    // Frame 0 should not be retrievable (closest would be frame 3)
    std::vector<uint8_t> out;
    FrameMetadata out_meta;
    EXPECT_TRUE(buf.copy_nearest(1000000, out, out_meta));
    EXPECT_EQ(out_meta.sequence, 3);  // Not 0
}

TEST_F(RawRingBufferTest, CopyNearestEmptyReturnsFalse) {
    RawRingBuffer buf(1, 10, FRAME_SIZE);
    std::vector<uint8_t> out;
    FrameMetadata out_meta;
    EXPECT_FALSE(buf.copy_nearest(1000000, out, out_meta));
}

TEST_F(RawRingBufferTest, Clear) {
    RawRingBuffer buf(1, 10, FRAME_SIZE);

    auto frame = make_frame(0xFF);
    buf.push(frame.data(), frame.size(), make_meta(0, 1000000));
    EXPECT_EQ(buf.size(), 1);

    buf.clear();
    EXPECT_EQ(buf.size(), 0);

    std::vector<uint8_t> out;
    FrameMetadata out_meta;
    EXPECT_FALSE(buf.copy_nearest(1000000, out, out_meta));
}

TEST_F(RawRingBufferTest, Stats) {
    RawRingBuffer buf(2, 10, FRAME_SIZE);

    for (int i = 0; i < 5; ++i) {
        auto frame = make_frame(static_cast<uint8_t>(i));
        buf.push(frame.data(), frame.size(), make_meta(i, 1000000 + i * 100000));
    }

    auto stats = buf.get_stats();
    EXPECT_EQ(stats.frame_count, 5);
    EXPECT_EQ(stats.total_bytes, 5 * FRAME_SIZE);
    EXPECT_EQ(stats.slot_capacity, 20);
    EXPECT_EQ(stats.oldest_timestamp_us, 1000000);
    EXPECT_EQ(stats.newest_timestamp_us, 1400000);
}

TEST_F(RawRingBufferTest, ThreadSafety) {
    RawRingBuffer buf(2, 30, FRAME_SIZE);
    std::atomic<bool> done{false};

    // Writer thread
    std::thread writer([&]() {
        for (int i = 0; i < 200; ++i) {
            auto frame = make_frame(static_cast<uint8_t>(i & 0xFF));
            buf.push(frame.data(), frame.size(),
                     make_meta(i, 1000000 + i * 33333));
        }
        done = true;
    });

    // Reader thread
    std::thread reader([&]() {
        std::vector<uint8_t> out;
        FrameMetadata out_meta;
        while (!done.load()) {
            buf.copy_nearest(2000000, out, out_meta);
        }
    });

    writer.join();
    reader.join();

    EXPECT_GT(buf.size(), 0);
    EXPECT_LE(buf.size(), buf.capacity());
}
