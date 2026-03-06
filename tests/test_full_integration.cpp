#include <gtest/gtest.h>
#include "camera_daemon/capture_pipeline.hpp"
#include "camera_daemon/camera_manager.hpp"
#include "camera_daemon/rtsp_server.hpp"
#include <libcamera/libcamera.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <set>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>

using namespace camera_daemon;

// ========== Hardware Resource Lock ==========
// Ensures only one test can access the camera at a time, even when ctest runs in parallel.
// This follows the design principle: "only ONE process may own the camera".

class HardwareLock {
public:
    HardwareLock() : fd_(-1) {}
    
    ~HardwareLock() {
        release();
    }
    
    bool acquire() {
        fd_ = open("/tmp/camera_test_lock", O_CREAT | O_RDWR, 0666);
        if (fd_ < 0) {
            return false;
        }
        // Blocking lock - waits for other tests to finish
        if (flock(fd_, LOCK_EX) != 0) {
            close(fd_);
            fd_ = -1;
            return false;
        }
        return true;
    }
    
    void release() {
        if (fd_ >= 0) {
            flock(fd_, LOCK_UN);
            close(fd_);
            fd_ = -1;
        }
    }
    
private:
    int fd_;
};

// ========== Camera Manager Integration Tests ==========

class CameraManagerIntegrationTest : public ::testing::Test {
protected:
    bool has_camera = false;
    HardwareLock hw_lock;
    
    void SetUp() override {
        // Acquire hardware lock FIRST - blocks until available
        ASSERT_TRUE(hw_lock.acquire()) << "Failed to acquire hardware lock";
        
        // Check if a camera is available
        libcamera::CameraManager cm;
        if (cm.start() == 0) {
            has_camera = !cm.cameras().empty();
            cm.stop();
        }
    }
    
    void TearDown() override {
        hw_lock.release();
    }
};

TEST_F(CameraManagerIntegrationTest, InitializeDetectsCamera) {
    if (!has_camera) {
        GTEST_SKIP() << "No camera available";
    }
    
    CameraManager manager;
    EXPECT_TRUE(manager.initialize());
}

TEST_F(CameraManagerIntegrationTest, ConfigureWithDefaults) {
    if (!has_camera) {
        GTEST_SKIP() << "No camera available";
    }
    
    CameraManager manager;
    ASSERT_TRUE(manager.initialize());
    
    CameraConfig config;
    config.width = 1280;
    config.height = 960;
    config.framerate = 30;
    
    EXPECT_TRUE(manager.configure(config));
}

TEST_F(CameraManagerIntegrationTest, StartStopCapture) {
    if (!has_camera) {
        GTEST_SKIP() << "No camera available";
    }
    
    CameraManager manager;
    ASSERT_TRUE(manager.initialize());
    
    CameraConfig config;
    config.width = 1280;
    config.height = 960;
    config.framerate = 30;
    ASSERT_TRUE(manager.configure(config));
    
    EXPECT_TRUE(manager.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    manager.stop();
}

TEST_F(CameraManagerIntegrationTest, ReceivesFrames) {
    if (!has_camera) {
        GTEST_SKIP() << "No camera available";
    }
    
    CameraManager manager;
    ASSERT_TRUE(manager.initialize());
    
    CameraConfig config;
    config.width = 1280;
    config.height = 960;
    config.framerate = 30;
    ASSERT_TRUE(manager.configure(config));
    
    std::atomic<int> frame_count{0};
    FrameMetadata last_meta{};
    
    manager.set_frame_callback([&](const FrameMetadata& meta, const uint8_t* data, size_t size) {
        frame_count++;
        last_meta = meta;
        EXPECT_NE(data, nullptr);
        EXPECT_GT(size, 0);
    });
    
    ASSERT_TRUE(manager.start());
    
    // Wait for some frames
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    manager.stop();
    
    EXPECT_GT(frame_count.load(), 5) << "Should receive at least 5 frames in 500ms";
    EXPECT_EQ(last_meta.width, 1280);
    EXPECT_EQ(last_meta.height, 960);
}

TEST_F(CameraManagerIntegrationTest, ProvidesDmabufFd) {
    if (!has_camera) {
        GTEST_SKIP() << "No camera available";
    }
    
    CameraManager manager;
    ASSERT_TRUE(manager.initialize());
    
    CameraConfig config;
    config.width = 1280;
    config.height = 720;
    config.framerate = 30;
    ASSERT_TRUE(manager.configure(config));
    
    int dmabuf_fd = -1;
    
    manager.set_frame_callback([&](const FrameMetadata& meta, const uint8_t*, size_t) {
        if (dmabuf_fd < 0) {
            dmabuf_fd = meta.dmabuf_fd;
        }
    });
    
    ASSERT_TRUE(manager.start());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    manager.stop();
    
    // libcamera should provide DMABUF file descriptors
    EXPECT_GE(dmabuf_fd, 0) << "Should provide valid DMABUF fd for zero-copy";
}

// ========== Capture Pipeline Integration Tests ==========

class CapturePipelineIntegrationTest : public ::testing::Test {
protected:
    bool has_camera = false;
    std::string temp_dir;
    HardwareLock hw_lock;
    
    void SetUp() override {
        // Acquire hardware lock FIRST - blocks until available
        ASSERT_TRUE(hw_lock.acquire()) << "Failed to acquire hardware lock";
        
        libcamera::CameraManager cm;
        if (cm.start() == 0) {
            has_camera = !cm.cameras().empty();
            cm.stop();
        }
        
        temp_dir = "/tmp/pipeline_test_" + std::to_string(getpid());
        std::filesystem::create_directories(temp_dir);
        std::filesystem::create_directories(temp_dir + "/stills");
        std::filesystem::create_directories(temp_dir + "/clips");
    }
    
    void TearDown() override {
        std::filesystem::remove_all(temp_dir);
        hw_lock.release();
    }
    
    DaemonConfig create_test_config() {
        DaemonConfig config;
        config.camera.width = 1280;
        config.camera.height = 960;
        config.camera.framerate = 30;
        config.camera.bitrate = 4000000;
        config.camera.keyframe_interval = 30;
        config.camera.jpeg_quality = 85;
        
        config.stills_dir = temp_dir + "/stills";
        config.clips_dir = temp_dir + "/clips";
        config.ring_buffer_seconds = 10;
        config.post_event_seconds = 3;
        
        config.enable_rtsp = false;  // Disable for basic tests
        config.enable_raw_sharing = false;
        config.enable_bgr_sharing = false;
        
        return config;
    }
};

TEST_F(CapturePipelineIntegrationTest, Initialize) {
    if (!has_camera) {
        GTEST_SKIP() << "No camera available";
    }
    
    auto config = create_test_config();
    CapturePipeline pipeline(config);
    
    EXPECT_TRUE(pipeline.initialize());
}

TEST_F(CapturePipelineIntegrationTest, StartStop) {
    if (!has_camera) {
        GTEST_SKIP() << "No camera available";
    }
    
    auto config = create_test_config();
    CapturePipeline pipeline(config);
    
    ASSERT_TRUE(pipeline.initialize());
    EXPECT_TRUE(pipeline.start());
    EXPECT_TRUE(pipeline.is_running());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    pipeline.stop();
    EXPECT_FALSE(pipeline.is_running());
}

TEST_F(CapturePipelineIntegrationTest, CapturesFrames) {
    if (!has_camera) {
        GTEST_SKIP() << "No camera available";
    }
    
    auto config = create_test_config();
    CapturePipeline pipeline(config);
    
    ASSERT_TRUE(pipeline.initialize());
    ASSERT_TRUE(pipeline.start());
    
    // Let it run for 2 seconds to capture and encode frames
    std::this_thread::sleep_for(std::chrono::seconds(2));
    
    auto stats = pipeline.get_stats();
    
    pipeline.stop();
    
    EXPECT_GT(stats.frames_captured, 30) << "Should capture at least 30 frames in 2s";
    EXPECT_GT(stats.frames_encoded, 30) << "Should encode at least 30 frames in 2s";
    EXPECT_GT(stats.ring_buffer_frames, 0) << "Ring buffer should have frames";
}

TEST_F(CapturePipelineIntegrationTest, CaptureStill) {
    if (!has_camera) {
        GTEST_SKIP() << "No camera available";
    }
    
    auto config = create_test_config();
    CapturePipeline pipeline(config);
    
    ASSERT_TRUE(pipeline.initialize());
    ASSERT_TRUE(pipeline.start());
    
    // Wait for stable capture
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Capture still
    std::string path = pipeline.capture_still();
    
    pipeline.stop();
    
    ASSERT_FALSE(path.empty()) << "Should return path to captured image";
    EXPECT_TRUE(std::filesystem::exists(path)) << "Image file should exist";
    
    // Verify JPEG
    std::ifstream file(path, std::ios::binary);
    uint8_t magic[2];
    file.read(reinterpret_cast<char*>(magic), 2);
    EXPECT_EQ(magic[0], 0xFF);
    EXPECT_EQ(magic[1], 0xD8);
}

TEST_F(CapturePipelineIntegrationTest, CaptureMultipleStills) {
    if (!has_camera) {
        GTEST_SKIP() << "No camera available";
    }
    
    auto config = create_test_config();
    CapturePipeline pipeline(config);
    
    ASSERT_TRUE(pipeline.initialize());
    ASSERT_TRUE(pipeline.start());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    
    std::vector<std::string> paths;
    for (int i = 0; i < 5; ++i) {
        std::string path = pipeline.capture_still();
        if (!path.empty()) {
            paths.push_back(path);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
    
    pipeline.stop();
    
    EXPECT_GE(paths.size(), 4) << "Should capture at least 4 of 5 stills";
    
    // All files should be unique
    std::set<std::string> unique_paths(paths.begin(), paths.end());
    EXPECT_EQ(unique_paths.size(), paths.size()) << "Each capture should produce unique file";
}

TEST_F(CapturePipelineIntegrationTest, CaptureClip) {
    if (!has_camera) {
        GTEST_SKIP() << "No camera available";
    }
    
    auto config = create_test_config();
    config.post_event_seconds = 2;  // Short post-event for faster test
    CapturePipeline pipeline(config);
    
    ASSERT_TRUE(pipeline.initialize());
    ASSERT_TRUE(pipeline.start());
    
    // Wait for ring buffer to fill
    std::this_thread::sleep_for(std::chrono::seconds(3));
    
    // Request clip from 2s ago to 2s from now (4s total)
    std::string path = pipeline.capture_clip(-2, 2);
    
    pipeline.stop();
    
    ASSERT_FALSE(path.empty()) << "Should return path to clip";
    EXPECT_TRUE(std::filesystem::exists(path)) << "Clip file should exist";
    
    auto size = std::filesystem::file_size(path);
    EXPECT_GT(size, 10000) << "Clip should have substantial content";
}

TEST_F(CapturePipelineIntegrationTest, GetStatus) {
    if (!has_camera) {
        GTEST_SKIP() << "No camera available";
    }
    
    auto config = create_test_config();
    CapturePipeline pipeline(config);
    
    ASSERT_TRUE(pipeline.initialize());
    ASSERT_TRUE(pipeline.start());
    
    std::this_thread::sleep_for(std::chrono::seconds(1));
    
    std::string status = pipeline.get_status_json();
    
    pipeline.stop();
    
    EXPECT_FALSE(status.empty());
    EXPECT_NE(status.find("running"), std::string::npos);
    EXPECT_NE(status.find("capture"), std::string::npos);
    EXPECT_NE(status.find("encoder"), std::string::npos);
}

TEST_F(CapturePipelineIntegrationTest, WithBgrSharing) {
    if (!has_camera) {
        GTEST_SKIP() << "No camera available";
    }
    
    auto config = create_test_config();
    config.enable_bgr_sharing = true;
    config.bgr_shm_name = "/test_bgr_frames";
    CapturePipeline pipeline(config);
    
    ASSERT_TRUE(pipeline.initialize());
    ASSERT_TRUE(pipeline.start());
    
    // Wait for frames to be published
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Try to subscribe
    FrameSubscriber subscriber(config.bgr_shm_name);
    
    std::vector<uint8_t> frame_data;
    FrameMetadata meta;
    
    bool got_frame = subscriber.read_frame(frame_data, meta, 1000);
    
    pipeline.stop();
    
    EXPECT_TRUE(got_frame) << "Should receive BGR frame via shared memory";
    if (got_frame) {
        EXPECT_EQ(meta.format, PIXFMT_BGR24);
        EXPECT_EQ(frame_data.size(), meta.width * meta.height * 3);
    }
}

// ========== RTSP Server Integration Tests ==========

class RTSPServerIntegrationTest : public ::testing::Test {
protected:
    bool has_camera = false;
    HardwareLock hw_lock;
    
    void SetUp() override {
        // Acquire hardware lock for tests that use camera
        // (StartStop and PushFrames don't need camera, but FullPipelineWithRTSP does)
    }
    
    void TearDown() override {
        hw_lock.release();
    }
    
    bool acquire_camera() {
        if (!hw_lock.acquire()) {
            return false;
        }
        libcamera::CameraManager cm;
        if (cm.start() == 0) {
            has_camera = !cm.cameras().empty();
            cm.stop();
        }
        return has_camera;
    }
};

TEST_F(RTSPServerIntegrationTest, StartStop) {
    RTSPServer::Config config;
    config.port = 18554;  // Non-standard port for testing
    config.width = 1280;
    config.height = 720;
    config.framerate = 30;
    
    RTSPServer server(config);
    
    EXPECT_TRUE(server.start());
    EXPECT_TRUE(server.is_running());
    
    server.stop();
    EXPECT_FALSE(server.is_running());
}

TEST_F(RTSPServerIntegrationTest, PushFrames) {
    RTSPServer::Config config;
    config.port = 18555;
    config.width = 1280;
    config.height = 720;
    config.framerate = 30;
    
    RTSPServer server(config);
    ASSERT_TRUE(server.start());
    
    // Create fake H.264 frames
    for (int i = 0; i < 30; ++i) {
        EncodedFrame frame;
        frame.metadata.width = 1280;
        frame.metadata.height = 720;
        frame.metadata.is_keyframe = (i == 0);
        frame.metadata.timestamp_us = i * 33333;
        
        // Fake NAL unit
        frame.data = {0x00, 0x00, 0x00, 0x01, static_cast<uint8_t>(i == 0 ? 0x67 : 0x41)};
        frame.data.resize(5000, 0x55);
        
        server.push_frame(frame);
    }
    
    server.stop();
    
    // If we got here without crashing, server handles frames correctly
    SUCCEED();
}

TEST_F(RTSPServerIntegrationTest, FullPipelineWithRTSP) {
    if (!acquire_camera()) {
        GTEST_SKIP() << "No camera available";
    }
    
    // Small delay to ensure libcamera fully releases resources
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    
    std::string temp_dir = "/tmp/rtsp_test_" + std::to_string(getpid());
    std::filesystem::create_directories(temp_dir);
    std::filesystem::create_directories(temp_dir + "/stills");
    std::filesystem::create_directories(temp_dir + "/clips");
    
    DaemonConfig config;
    config.camera.width = 1280;
    config.camera.height = 720;
    config.camera.framerate = 30;
    config.camera.bitrate = 2000000;
    config.camera.keyframe_interval = 30;
    
    config.stills_dir = temp_dir + "/stills";
    config.clips_dir = temp_dir + "/clips";
    config.ring_buffer_seconds = 5;
    
    config.enable_rtsp = true;
    config.rtsp_port = 18556;
    
    CapturePipeline pipeline(config);
    
    ASSERT_TRUE(pipeline.initialize());
    ASSERT_TRUE(pipeline.start());
    
    // Run for 3 seconds with RTSP server active
    std::this_thread::sleep_for(std::chrono::seconds(3));
    
    auto stats = pipeline.get_stats();
    
    pipeline.stop();
    
    std::filesystem::remove_all(temp_dir);
    
    EXPECT_GT(stats.frames_captured, 60);
    EXPECT_GT(stats.frames_encoded, 60);
}

// ========== Stress Tests ==========

TEST_F(CapturePipelineIntegrationTest, StressTestMultipleStills) {
    if (!has_camera) {
        GTEST_SKIP() << "No camera available";
    }
    
    auto config = create_test_config();
    CapturePipeline pipeline(config);
    
    ASSERT_TRUE(pipeline.initialize());
    ASSERT_TRUE(pipeline.start());
    
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    
    // Rapid-fire still captures
    std::vector<std::string> paths;
    for (int i = 0; i < 20; ++i) {
        std::string path = pipeline.capture_still();
        if (!path.empty()) {
            paths.push_back(path);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));  // 20fps capture rate
    }
    
    pipeline.stop();
    
    EXPECT_GE(paths.size(), 15) << "Should capture at least 15 of 20 rapid stills";
}

TEST_F(CapturePipelineIntegrationTest, LongRunningCapture) {
    if (!has_camera) {
        GTEST_SKIP() << "No camera available";
    }
    
    auto config = create_test_config();
    CapturePipeline pipeline(config);
    
    ASSERT_TRUE(pipeline.initialize());
    ASSERT_TRUE(pipeline.start());
    
    // Run for 10 seconds
    std::this_thread::sleep_for(std::chrono::seconds(10));
    
    auto stats = pipeline.get_stats();
    
    pipeline.stop();
    
    // At 30fps, expect ~300 frames in 10 seconds
    EXPECT_GT(stats.frames_captured, 250);
    EXPECT_GT(stats.frames_encoded, 250);
    
    // Very few dropped frames
    double drop_rate = static_cast<double>(stats.frames_dropped) / stats.frames_captured;
    EXPECT_LT(drop_rate, 0.05) << "Drop rate should be under 5%";
}
