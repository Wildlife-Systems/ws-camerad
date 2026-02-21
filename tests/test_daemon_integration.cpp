/**
 * Integration tests that connect to a SINGLE daemon instance as real clients would.
 * 
 * This follows the design principle: "only ONE process may own the camera".
 * All tests use ControlClient to interact with the daemon via the Unix socket,
 * exactly as external consumers would in production.
 */

#include <gtest/gtest.h>
#include "camera_daemon/capture_pipeline.hpp"
#include "camera_daemon/control_socket.hpp"
#include "camera_daemon/daemon.hpp"
#include <libcamera/libcamera.h>
#include <filesystem>
#include <fstream>
#include <thread>
#include <chrono>
#include <memory>
#include <atomic>

using namespace camera_daemon;

// Simple JSON path extractor (looks for "path":"..." pattern)
static std::string extract_path_from_json(const std::string& json) {
    const std::string key = "\"path\":\"";
    auto pos = json.find(key);
    if (pos == std::string::npos) return "";
    pos += key.length();
    auto end = json.find("\"", pos);
    if (end == std::string::npos) return "";
    return json.substr(pos, end - pos);
}

// ========== Shared Daemon Fixture ==========
// Starts ONE daemon instance that all tests share, accessed via ControlClient

class SharedDaemonTest : public ::testing::Test {
protected:
    static bool has_camera_;
    static std::string temp_dir_;
    static std::string socket_path_;
    static std::unique_ptr<CapturePipeline> pipeline_;
    static std::unique_ptr<ControlSocket> control_socket_;
    static std::atomic<bool> daemon_running_;
    
    static void SetUpTestSuite() {
        // Check for camera - must be done BEFORE creating pipeline
        // and the CameraManager must be fully destroyed before pipeline creates its own
        {
            libcamera::CameraManager cm;
            if (cm.start() == 0) {
                auto cameras = cm.cameras();
                if (!cameras.empty()) {
                    auto camera = cameras[0];
                    if (camera->acquire() == 0) {
                        has_camera_ = true;
                        camera->release();
                    }
                }
                cm.stop();
            }
        }  // cm destroyed here
        
        // Small delay to ensure libcamera fully releases resources
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        
        if (!has_camera_) {
            return;
        }
        
        // Create temp directories
        temp_dir_ = "/tmp/daemon_test_" + std::to_string(getpid());
        socket_path_ = temp_dir_ + "/camera.sock";
        std::filesystem::create_directories(temp_dir_);
        std::filesystem::create_directories(temp_dir_ + "/stills");
        std::filesystem::create_directories(temp_dir_ + "/clips");
        
        // Create daemon config
        DaemonConfig config;
        config.camera.width = 1280;
        config.camera.height = 960;
        config.camera.framerate = 30;
        config.camera.bitrate = 4000000;
        config.camera.keyframe_interval = 30;
        config.camera.jpeg_quality = 85;
        
        config.stills_dir = temp_dir_ + "/stills";
        config.clips_dir = temp_dir_ + "/clips";
        config.socket_path = socket_path_;
        config.ring_buffer_seconds = 10;
        config.post_event_seconds = 3;
        
        config.enable_rtsp = false;
        config.enable_raw_sharing = false;
        config.enable_bgr_sharing = false;
        
        // Start the ONE daemon instance
        pipeline_ = std::make_unique<CapturePipeline>(config);
        if (!pipeline_->initialize()) {
            std::cerr << "Failed to initialize pipeline" << std::endl;
            pipeline_.reset();
            return;
        }
        
        // Start control socket
        control_socket_ = std::make_unique<ControlSocket>(socket_path_);
        
        // Wire up command handlers
        control_socket_->set_still_handler([](const Command& cmd) {
            std::string path = pipeline_->capture_still(cmd.int_value);
            if (path.empty()) {
                return Response::error("Still capture failed");
            }
            return Response::ok_path(path);
        });
        
        control_socket_->set_clip_handler([](const Command& cmd) {
            std::string path = pipeline_->capture_clip(cmd.int_value, cmd.int_value2);
            if (path.empty()) {
                return Response::error("Clip extraction failed");
            }
            return Response::ok_path(path);
        });
        
        control_socket_->set_get_handler([](const Command& cmd) {
            if (cmd.key == "STATUS") {
                return Response::ok_data(pipeline_->get_status_json());
            }
            return Response::error("Unknown key: " + cmd.key);
        });
        
        if (!control_socket_->start()) {
            std::cerr << "Failed to start control socket" << std::endl;
            pipeline_.reset();
            control_socket_.reset();
            return;
        }
        
        if (!pipeline_->start()) {
            std::cerr << "Failed to start pipeline" << std::endl;
            control_socket_->stop();
            pipeline_.reset();
            control_socket_.reset();
            return;
        }
        
        daemon_running_ = true;
        
        // Give daemon time to start capturing
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    static void TearDownTestSuite() {
        if (pipeline_) {
            pipeline_->stop();
            pipeline_.reset();
        }
        if (control_socket_) {
            control_socket_->stop();
            control_socket_.reset();
        }
        daemon_running_ = false;
        
        if (!temp_dir_.empty()) {
            std::filesystem::remove_all(temp_dir_);
        }
    }
    
    void SetUp() override {
        if (!has_camera_) {
            GTEST_SKIP() << "No camera available";
        }
        if (!daemon_running_) {
            GTEST_SKIP() << "Daemon failed to start";
        }
    }
    
    // Helper to create a client connection
    std::unique_ptr<ControlClient> create_client() {
        auto client = std::make_unique<ControlClient>(socket_path_);
        EXPECT_TRUE(client->connect()) << "Client should connect to daemon";
        return client;
    }
};

// Static member definitions
bool SharedDaemonTest::has_camera_ = false;
std::string SharedDaemonTest::temp_dir_;
std::string SharedDaemonTest::socket_path_;
std::unique_ptr<CapturePipeline> SharedDaemonTest::pipeline_;
std::unique_ptr<ControlSocket> SharedDaemonTest::control_socket_;
std::atomic<bool> SharedDaemonTest::daemon_running_{false};

// ========== Consumer Tests ==========
// These tests act as external consumers connecting to the daemon

TEST_F(SharedDaemonTest, ClientCanConnect) {
    auto client = create_client();
    EXPECT_TRUE(client->is_connected());
}

TEST_F(SharedDaemonTest, MultipleClientsCanConnect) {
    auto client1 = create_client();
    auto client2 = create_client();
    auto client3 = create_client();
    
    EXPECT_TRUE(client1->is_connected());
    EXPECT_TRUE(client2->is_connected());
    EXPECT_TRUE(client3->is_connected());
}

TEST_F(SharedDaemonTest, CaptureStillViaSocket) {
    auto client = create_client();
    
    auto response = client->capture_still();
    
    EXPECT_TRUE(response.success) << "Still capture should succeed: " << response.json;
    
    // Extract path from JSON response
    if (response.success) {
        std::string path = extract_path_from_json(response.json);
        EXPECT_FALSE(path.empty()) << "Response should contain path";
        if (!path.empty()) {
            EXPECT_TRUE(std::filesystem::exists(path)) << "Image file should exist: " << path;
        }
    }
}

TEST_F(SharedDaemonTest, CaptureStillWithTimeOffset) {
    auto client = create_client();
    
    // Capture 2 seconds in future
    auto response = client->capture_still(2);
    
    EXPECT_TRUE(response.success) << "Still capture with delay should succeed";
}

TEST_F(SharedDaemonTest, CaptureClipViaSocket) {
    auto client = create_client();
    
    // Clip from 3 seconds ago to now (3 seconds total, all from buffer)
    auto response = client->capture_clip(-3, 0);
    
    EXPECT_TRUE(response.success) << "Clip capture should succeed: " << response.json;
    
    if (response.success) {
        std::string path = extract_path_from_json(response.json);
        EXPECT_FALSE(path.empty()) << "Response should contain path";
        if (!path.empty()) {
            EXPECT_TRUE(std::filesystem::exists(path)) << "Clip file should exist: " << path;
        }
    }
}

TEST_F(SharedDaemonTest, CaptureClipWithPostEvent) {
    auto client = create_client();
    
    // Clip from 2 seconds ago to 2 seconds from now (4 seconds total)
    auto response = client->capture_clip(-2, 2);
    
    EXPECT_TRUE(response.success) << "Clip with post-event should succeed: " << response.json;
}

TEST_F(SharedDaemonTest, GetStatusViaSocket) {
    auto client = create_client();
    
    auto response = client->get_status();
    
    EXPECT_TRUE(response.success) << "Status request should succeed";
    EXPECT_FALSE(response.json.empty()) << "Should return status JSON";
    
    // Should contain JSON with data field
    EXPECT_NE(response.json.find("\"data\":"), std::string::npos) << "Response should contain data";
    EXPECT_NE(response.json.find("\"running\":"), std::string::npos) << "Status should include running state";
}

TEST_F(SharedDaemonTest, ConcurrentStillRequests) {
    std::vector<std::thread> threads;
    std::atomic<int> success_count{0};
    
    // 5 concurrent clients requesting stills
    for (int i = 0; i < 5; ++i) {
        threads.emplace_back([this, &success_count]() {
            auto client = create_client();
            auto response = client->capture_still();
            if (response.success) {
                success_count++;
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(success_count.load(), 5) << "All 5 concurrent still requests should succeed";
}

TEST_F(SharedDaemonTest, ConcurrentMixedRequests) {
    std::vector<std::thread> threads;
    std::atomic<int> still_success{0};
    std::atomic<int> status_success{0};
    
    // Mix of still and status requests
    for (int i = 0; i < 4; ++i) {
        threads.emplace_back([this, &still_success, i]() {
            auto client = create_client();
            std::this_thread::sleep_for(std::chrono::milliseconds(i * 50));
            auto response = client->capture_still();
            if (response.success) {
                still_success++;
            }
        });
        
        threads.emplace_back([this, &status_success, i]() {
            auto client = create_client();
            std::this_thread::sleep_for(std::chrono::milliseconds(i * 50 + 25));
            auto response = client->get_status();
            if (response.success) {
                status_success++;
            }
        });
    }
    
    for (auto& t : threads) {
        t.join();
    }
    
    EXPECT_EQ(still_success.load(), 4) << "All still requests should succeed";
    EXPECT_EQ(status_success.load(), 4) << "All status requests should succeed";
}

TEST_F(SharedDaemonTest, RapidStillCapture) {
    auto client = create_client();
    
    int success_count = 0;
    for (int i = 0; i < 10; ++i) {
        auto response = client->capture_still();
        if (response.success) {
            success_count++;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    
    EXPECT_GE(success_count, 8) << "At least 8 of 10 rapid stills should succeed";
}

TEST_F(SharedDaemonTest, LongRunningClientConnection) {
    auto client = create_client();
    
    // Keep connection open, periodically request status
    for (int i = 0; i < 5; ++i) {
        auto response = client->get_status();
        EXPECT_TRUE(response.success) << "Request " << i << " should succeed";
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
    }
    
    // Should still work
    auto response = client->capture_still();
    EXPECT_TRUE(response.success) << "Final still capture should succeed";
}
