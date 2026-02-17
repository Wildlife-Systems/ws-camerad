#include <gtest/gtest.h>
#include "camera_daemon/logger.hpp"
#include <sstream>

using namespace camera_daemon;

class LoggerTest : public ::testing::Test {
protected:
    void SetUp() override {
        // Redirect logger output to our stringstream
        Logger::instance().set_output(output_);
        Logger::instance().set_level(LogLevel::DEBUG);
    }

    void TearDown() override {
        // Reset to stdout
        Logger::instance().set_output(std::cout);
        Logger::instance().set_level(LogLevel::INFO);
    }

    std::string get_output() {
        return output_.str();
    }

    void clear_output() {
        output_.str("");
        output_.clear();
    }

    std::ostringstream output_;
};

TEST_F(LoggerTest, LogsDebugWhenEnabled) {
    LOG_DEBUG("debug message");
    EXPECT_TRUE(get_output().find("debug message") != std::string::npos);
    EXPECT_TRUE(get_output().find("[DEBUG]") != std::string::npos);
}

TEST_F(LoggerTest, LogsInfo) {
    LOG_INFO("info message");
    EXPECT_TRUE(get_output().find("info message") != std::string::npos);
    EXPECT_TRUE(get_output().find("[INFO ]") != std::string::npos);
}

TEST_F(LoggerTest, LogsWarn) {
    LOG_WARN("warning message");
    EXPECT_TRUE(get_output().find("warning message") != std::string::npos);
    EXPECT_TRUE(get_output().find("[WARN ]") != std::string::npos);
}

TEST_F(LoggerTest, LogsError) {
    LOG_ERROR("error message");
    EXPECT_TRUE(get_output().find("error message") != std::string::npos);
    EXPECT_TRUE(get_output().find("[ERROR]") != std::string::npos);
}

TEST_F(LoggerTest, FiltersByLevel) {
    Logger::instance().set_level(LogLevel::WARN);
    
    LOG_DEBUG("debug");
    LOG_INFO("info");
    LOG_WARN("warning");
    LOG_ERROR("error");
    
    std::string out = get_output();
    EXPECT_TRUE(out.find("debug") == std::string::npos);
    EXPECT_TRUE(out.find("info") == std::string::npos);
    EXPECT_TRUE(out.find("warning") != std::string::npos);
    EXPECT_TRUE(out.find("error") != std::string::npos);
}

TEST_F(LoggerTest, LogsMultipleArgs) {
    LOG_INFO("value=", 42, ", name=", "test");
    std::string out = get_output();
    EXPECT_TRUE(out.find("value=42") != std::string::npos);
    EXPECT_TRUE(out.find("name=test") != std::string::npos);
}

TEST_F(LoggerTest, IncludesSourceFile) {
    LOG_INFO("test");
    std::string out = get_output();
    // Should include the test filename
    EXPECT_TRUE(out.find("test_logger.cpp") != std::string::npos);
}

TEST_F(LoggerTest, IncludesTimestamp) {
    LOG_INFO("test");
    std::string out = get_output();
    // Should include date/time pattern like "2026-02-06"
    EXPECT_TRUE(out.find("-") != std::string::npos);
    EXPECT_TRUE(out.find(":") != std::string::npos);
}
