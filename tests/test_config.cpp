#include <gtest/gtest.h>
#include "camera_daemon/common.hpp"
#include "camera_daemon/daemon.hpp"
#include <fstream>
#include <filesystem>

namespace camera_daemon {
// Forward declaration of internal function for testing
DaemonConfig load_config(const std::string& path);
}

using namespace camera_daemon;

class ConfigTest : public ::testing::Test {
protected:
    std::string temp_dir;
    
    void SetUp() override {
        temp_dir = "/tmp/camera_test_" + std::to_string(getpid());
        std::filesystem::create_directories(temp_dir);
    }
    
    void TearDown() override {
        std::filesystem::remove_all(temp_dir);
    }
    
    std::string write_config(const std::string& content) {
        std::string path = temp_dir + "/test.conf";
        std::ofstream f(path);
        f << content;
        f.close();
        return path;
    }
};

TEST_F(ConfigTest, DefaultsWhenFileNotFound) {
    DaemonConfig config = load_config("/nonexistent/path/config.conf");
    
    // Should have all defaults
    EXPECT_EQ(config.socket_path, DEFAULT_SOCKET_PATH);
    EXPECT_EQ(config.stills_dir, DEFAULT_STILLS_DIR);
    EXPECT_EQ(config.clips_dir, DEFAULT_CLIPS_DIR);
    EXPECT_EQ(config.camera.width, 1280);
    EXPECT_EQ(config.camera.height, 960);
}

TEST_F(ConfigTest, ParsesDaemonSection) {
    auto path = write_config(R"(
[daemon]
socket_path = /run/test/socket.sock
stills_dir = /data/stills
clips_dir = /data/clips
ring_buffer_seconds = 60
post_event_seconds = 20
enable_rtsp = false
rtsp_port = 9000
)");
    
    DaemonConfig config = load_config(path);
    
    EXPECT_EQ(config.socket_path, "/run/test/socket.sock");
    EXPECT_EQ(config.stills_dir, "/data/stills");
    EXPECT_EQ(config.clips_dir, "/data/clips");
    EXPECT_EQ(config.ring_buffer_seconds, 60);
    EXPECT_EQ(config.post_event_seconds, 20);
    EXPECT_FALSE(config.enable_rtsp);
    EXPECT_EQ(config.rtsp_port, 9000);
}

TEST_F(ConfigTest, ParsesCameraSection) {
    auto path = write_config(R"(
[camera]
width = 1920
height = 1080
framerate = 60
bitrate = 8000000
keyframe_interval = 60
jpeg_quality = 95
)");
    
    DaemonConfig config = load_config(path);
    
    EXPECT_EQ(config.camera.width, 1920);
    EXPECT_EQ(config.camera.height, 1080);
    EXPECT_EQ(config.camera.framerate, 60);
    EXPECT_EQ(config.camera.bitrate, 8000000);
    EXPECT_EQ(config.camera.keyframe_interval, 60);
    EXPECT_EQ(config.camera.jpeg_quality, 95);
}

TEST_F(ConfigTest, IgnoresComments) {
    auto path = write_config(R"(
# This is a comment
; This is also a comment
[daemon]
socket_path = /run/test.sock
# rtsp_port = 1234
)");
    
    DaemonConfig config = load_config(path);
    
    EXPECT_EQ(config.socket_path, "/run/test.sock");
    EXPECT_EQ(config.rtsp_port, 8554);  // Default, not 1234
}

TEST_F(ConfigTest, IgnoresEmptyLines) {
    auto path = write_config(R"(

[daemon]

socket_path = /run/test.sock

[camera]

width = 1920

)");
    
    DaemonConfig config = load_config(path);
    
    EXPECT_EQ(config.socket_path, "/run/test.sock");
    EXPECT_EQ(config.camera.width, 1920);
}

TEST_F(ConfigTest, TrimsWhitespace) {
    auto path = write_config(R"(
[daemon]
socket_path =   /run/test.sock   
  stills_dir   =   /data/stills   
)");
    
    DaemonConfig config = load_config(path);
    
    EXPECT_EQ(config.socket_path, "/run/test.sock");
    EXPECT_EQ(config.stills_dir, "/data/stills");
}

TEST_F(ConfigTest, ParsesBooleans) {
    // Test "true"
    {
        auto path = write_config("[daemon]\nenable_rtsp = true\n");
        DaemonConfig config = load_config(path);
        EXPECT_TRUE(config.enable_rtsp);
    }
    
    // Test "1"
    {
        auto path = write_config("[daemon]\nenable_rtsp = 1\n");
        DaemonConfig config = load_config(path);
        EXPECT_TRUE(config.enable_rtsp);
    }
    
    // Test "false"
    {
        auto path = write_config("[daemon]\nenable_rtsp = false\n");
        DaemonConfig config = load_config(path);
        EXPECT_FALSE(config.enable_rtsp);
    }
    
    // Test "0"
    {
        auto path = write_config("[daemon]\nenable_rtsp = 0\n");
        DaemonConfig config = load_config(path);
        EXPECT_FALSE(config.enable_rtsp);
    }
}

TEST_F(ConfigTest, HandlesInvalidLines) {
    auto path = write_config(R"(
[daemon]
socket_path = /run/test.sock
this line has no equals sign
clips_dir = /data/clips
)");
    
    DaemonConfig config = load_config(path);
    
    // Should parse valid lines and skip invalid
    EXPECT_EQ(config.socket_path, "/run/test.sock");
    EXPECT_EQ(config.clips_dir, "/data/clips");
}

TEST_F(ConfigTest, HandlesUnknownKeys) {
    auto path = write_config(R"(
[daemon]
socket_path = /run/test.sock
unknown_key = some_value
another_unknown = 123
)");
    
    DaemonConfig config = load_config(path);
    
    // Should parse known keys and ignore unknown
    EXPECT_EQ(config.socket_path, "/run/test.sock");
}

TEST_F(ConfigTest, ParsesShmNames) {
    auto path = write_config(R"(
[daemon]
shm_name = /my_shm
bgr_shm_name = /my_bgr_shm
enable_raw_sharing = true
enable_bgr_sharing = true
)");
    
    DaemonConfig config = load_config(path);
    
    EXPECT_EQ(config.shm_name, "/my_shm");
    EXPECT_EQ(config.bgr_shm_name, "/my_bgr_shm");
    EXPECT_TRUE(config.enable_raw_sharing);
    EXPECT_TRUE(config.enable_bgr_sharing);
}

TEST_F(ConfigTest, DefaultSectionWithoutHeader) {
    // Keys without section header go to "daemon" section
    auto path = write_config(R"(
socket_path = /run/nosection.sock
stills_dir = /data/nostills
)");
    
    DaemonConfig config = load_config(path);
    
    EXPECT_EQ(config.socket_path, "/run/nosection.sock");
    EXPECT_EQ(config.stills_dir, "/data/nostills");
}
