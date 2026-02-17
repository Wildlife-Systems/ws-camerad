#include <gtest/gtest.h>
#include "camera_daemon/control_socket.hpp"

using namespace camera_daemon;

// Test Response struct (JSON format)
TEST(ResponseTest, OkResponse) {
    auto resp = Response::ok();
    EXPECT_TRUE(resp.success);
    EXPECT_EQ(resp.json, R"({"ok":true})");
    EXPECT_EQ(resp.to_string(), "{\"ok\":true}\n");
}

TEST(ResponseTest, OkResponseWithPath) {
    auto resp = Response::ok_path("/var/lib/ws-camerad/stills/test.jpg");
    EXPECT_TRUE(resp.success);
    EXPECT_EQ(resp.json, R"({"ok":true,"path":"/var/lib/ws-camerad/stills/test.jpg"})");
}

TEST(ResponseTest, OkResponseWithData) {
    auto resp = Response::ok_data(R"({"frames":100})");
    EXPECT_TRUE(resp.success);
    EXPECT_EQ(resp.json, R"({"ok":true,"data":{"frames":100}})");
}

TEST(ResponseTest, ErrorResponse) {
    auto resp = Response::error("file not found");
    EXPECT_FALSE(resp.success);
    EXPECT_EQ(resp.json, R"({"ok":false,"error":"file not found"})");
}

TEST(ResponseTest, EscapeSpecialChars) {
    auto resp = Response::error("path with \"quotes\" and\nnewline");
    EXPECT_FALSE(resp.success);
    // Should escape quotes and newlines
    EXPECT_NE(resp.json.find("\\\""), std::string::npos);
    EXPECT_NE(resp.json.find("\\n"), std::string::npos);
}

// Test Command struct defaults
TEST(CommandTest, DefaultValues) {
    Command cmd;
    cmd.type = CommandType::STILL;
    EXPECT_TRUE(cmd.key.empty());
    EXPECT_TRUE(cmd.value.empty());
    EXPECT_EQ(cmd.int_value, 0);
    EXPECT_EQ(cmd.int_value2, 0);
}

// Test CommandType enum values
TEST(CommandTypeTest, EnumValues) {
    EXPECT_NE(static_cast<int>(CommandType::STILL), static_cast<int>(CommandType::CLIP));
    EXPECT_NE(static_cast<int>(CommandType::SET), static_cast<int>(CommandType::GET));
    EXPECT_NE(static_cast<int>(CommandType::GET), static_cast<int>(CommandType::QUIT));
}
