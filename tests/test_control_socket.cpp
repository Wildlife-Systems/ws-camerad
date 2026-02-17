#include <gtest/gtest.h>
#include "camera_daemon/control_socket.hpp"
#include <filesystem>

using namespace camera_daemon;

// ========== Response Tests (additional) ==========

TEST(ResponseTest, ToStringFormat) {
    Response ok = Response::ok();
    EXPECT_EQ(ok.to_string(), "{\"ok\":true}\n");
    
    Response ok_path = Response::ok_path("file.jpg");
    EXPECT_EQ(ok_path.to_string(), "{\"ok\":true,\"path\":\"file.jpg\"}\n");
    
    Response err = Response::error("not found");
    EXPECT_EQ(err.to_string(), "{\"ok\":false,\"error\":\"not found\"}\n");
}

TEST(ResponseTest, SuccessFlag) {
    Response ok = Response::ok();
    EXPECT_TRUE(ok.success);
    
    Response err = Response::error("test");
    EXPECT_FALSE(err.success);
}

// ========== ControlClient Tests ==========

TEST(ControlClientTest, DefaultSocketPath) {
    ControlClient client;
    EXPECT_FALSE(client.is_connected());
}

TEST(ControlClientTest, CustomSocketPath) {
    ControlClient client("/custom/path.sock");
    EXPECT_FALSE(client.is_connected());
}

TEST(ControlClientTest, ConnectToNonexistentSocket) {
    ControlClient client("/tmp/nonexistent_" + std::to_string(getpid()) + ".sock");
    EXPECT_FALSE(client.connect());
    EXPECT_FALSE(client.is_connected());
}

TEST(ControlClientTest, DoubleDisconnect) {
    ControlClient client;
    // Disconnecting without connecting should be safe
    client.disconnect();
    client.disconnect();
    EXPECT_FALSE(client.is_connected());
}

// ========== CommandType Tests ==========

TEST(CommandTypeTest, AllTypesDistinct) {
    EXPECT_NE(CommandType::STILL, CommandType::CLIP);
    EXPECT_NE(CommandType::CLIP, CommandType::SET);
    EXPECT_NE(CommandType::SET, CommandType::GET);
    EXPECT_NE(CommandType::GET, CommandType::QUIT);
}

// ========== Integration Test with Unix Socket ==========

class ControlSocketTest : public ::testing::Test {
protected:
    std::string socket_path;
    
    void SetUp() override {
        socket_path = "/tmp/test_socket_" + std::to_string(getpid()) + ".sock";
        // Clean up any leftover socket
        std::filesystem::remove(socket_path);
    }
    
    void TearDown() override {
        std::filesystem::remove(socket_path);
    }
};

TEST_F(ControlSocketTest, ConstructDestruct) {
    { 
        ControlSocket server(socket_path);
        EXPECT_FALSE(server.is_running());
    }
    // Should not leave socket file after destruction without start
}

TEST_F(ControlSocketTest, StartStop) {
    ControlSocket server(socket_path);
    
    EXPECT_TRUE(server.start());
    EXPECT_TRUE(server.is_running());
    EXPECT_TRUE(std::filesystem::exists(socket_path));
    
    server.stop();
    EXPECT_FALSE(server.is_running());
}

TEST_F(ControlSocketTest, DoubleStart) {
    ControlSocket server(socket_path);
    
    EXPECT_TRUE(server.start());
    EXPECT_TRUE(server.start());  // Second start should be OK (no-op)
    EXPECT_TRUE(server.is_running());
    
    server.stop();
}

TEST_F(ControlSocketTest, DoubleStop) {
    ControlSocket server(socket_path);
    
    server.start();
    server.stop();
    server.stop();  // Second stop should be safe
    
    EXPECT_FALSE(server.is_running());
}

TEST_F(ControlSocketTest, ClientCountZeroInitially) {
    ControlSocket server(socket_path);
    server.start();
    
    EXPECT_EQ(server.client_count(), 0);
    
    server.stop();
}

TEST_F(ControlSocketTest, SetHandlers) {
    ControlSocket server(socket_path);
    
    bool still_called = false;
    bool clip_called = false;
    bool set_called = false;
    bool get_called = false;
    
    server.set_still_handler([&](const Command&) { still_called = true; return Response::ok(); });
    server.set_clip_handler([&](const Command&) { clip_called = true; return Response::ok(); });
    server.set_set_handler([&](const Command&) { set_called = true; return Response::ok(); });
    server.set_get_handler([&](const Command&) { get_called = true; return Response::ok(); });
    
    // Handlers are set but not called yet
    EXPECT_FALSE(still_called);
    EXPECT_FALSE(clip_called);
    EXPECT_FALSE(set_called);
    EXPECT_FALSE(get_called);
}

// Test complete client-server interaction
TEST_F(ControlSocketTest, ClientServerInteraction) {
    ControlSocket server(socket_path);
    
    std::string received_key;
    server.set_get_handler([&](const Command& cmd) {
        received_key = cmd.key;
        return Response::ok_data(R"({"value":"123"})");
    });
    
    ASSERT_TRUE(server.start());
    
    // Connect client
    ControlClient client(socket_path);
    ASSERT_TRUE(client.connect());
    EXPECT_TRUE(client.is_connected());
    
    // Send command
    Response resp = client.send_command("GET status\n");
    
    EXPECT_TRUE(resp.success);
    EXPECT_EQ(received_key, "STATUS");
    
    client.disconnect();
    server.stop();
}

TEST_F(ControlSocketTest, StillCommand) {
    ControlSocket server(socket_path);
    
    bool handler_called = false;
    server.set_still_handler([&](const Command& cmd) {
        handler_called = true;
        EXPECT_EQ(cmd.type, CommandType::STILL);
        return Response::ok_path("/path/to/image.jpg");
    });
    
    ASSERT_TRUE(server.start());
    
    ControlClient client(socket_path);
    ASSERT_TRUE(client.connect());
    
    Response resp = client.capture_still();
    EXPECT_TRUE(resp.success);
    EXPECT_TRUE(handler_called);
    
    client.disconnect();
    server.stop();
}

TEST_F(ControlSocketTest, ClipCommand) {
    ControlSocket server(socket_path);
    
    int received_pre = 0, received_post = 0;
    server.set_clip_handler([&](const Command& cmd) {
        received_pre = cmd.int_value;
        received_post = cmd.int_value2;
        return Response::ok_path("/path/to/clip.mp4");
    });
    
    ASSERT_TRUE(server.start());
    
    ControlClient client(socket_path);
    ASSERT_TRUE(client.connect());
    
    Response resp = client.capture_clip(10, 5);
    EXPECT_TRUE(resp.success);
    EXPECT_EQ(received_pre, 10);
    EXPECT_EQ(received_post, 5);
    
    client.disconnect();
    server.stop();
}

TEST_F(ControlSocketTest, SetCommand) {
    ControlSocket server(socket_path);
    
    std::string received_key, received_value;
    server.set_set_handler([&](const Command& cmd) {
        received_key = cmd.key;
        received_value = cmd.value;
        return Response::ok();
    });
    
    ASSERT_TRUE(server.start());
    
    ControlClient client(socket_path);
    ASSERT_TRUE(client.connect());
    
    Response resp = client.set_parameter("bitrate", "5000000");
    EXPECT_TRUE(resp.success);
    EXPECT_EQ(received_key, "bitrate");
    EXPECT_EQ(received_value, "5000000");
    
    client.disconnect();
    server.stop();
}

TEST_F(ControlSocketTest, InvalidCommand) {
    ControlSocket server(socket_path);
    ASSERT_TRUE(server.start());
    
    ControlClient client(socket_path);
    ASSERT_TRUE(client.connect());
    
    Response resp = client.send_command("INVALID_COMMAND\n");
    EXPECT_FALSE(resp.success);
    
    client.disconnect();
    server.stop();
}

TEST_F(ControlSocketTest, QuitCommand) {
    ControlSocket server(socket_path);
    ASSERT_TRUE(server.start());
    
    ControlClient client(socket_path);
    ASSERT_TRUE(client.connect());
    
    Response resp = client.send_command("QUIT\n");
    EXPECT_TRUE(resp.success);
    // JSON format: {"ok":true}
    EXPECT_NE(resp.json.find("\"ok\":true"), std::string::npos);
    
    client.disconnect();
    server.stop();
}

TEST_F(ControlSocketTest, CaseInsensitiveCommands) {
    ControlSocket server(socket_path);
    
    int call_count = 0;
    server.set_still_handler([&](const Command&) {
        call_count++;
        return Response::ok();
    });
    
    ASSERT_TRUE(server.start());
    
    ControlClient client(socket_path);
    ASSERT_TRUE(client.connect());
    
    client.send_command("still\n");
    client.send_command("STILL\n");
    client.send_command("Still\n");
    
    // Small delay to let server process
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    
    EXPECT_EQ(call_count, 3);
    
    client.disconnect();
    server.stop();
}
