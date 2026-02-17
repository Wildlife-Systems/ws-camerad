#pragma once

#include "common.hpp"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <functional>
#include <unordered_map>
#include <optional>

namespace camera_daemon {

/**
 * Command types supported by the control socket.
 */
enum class CommandType {
    STILL,      // Capture a still image
    BURST,      // Burst capture (multiple stills)
    CLIP,       // Extract video clip
    SET,        // Set camera parameter
    GET,        // Get status or parameter
    QUIT        // Disconnect
};

/**
 * Parsed command from client.
 */
struct Command {
    CommandType type;
    std::string key;
    std::string value;
    int int_value = 0;      // STILL: time offset; CLIP: start offset; BURST: count
    int int_value2 = 0;     // CLIP: end offset; BURST: interval_ms
};

/**
 * Response to send back to client (JSON format).
 */
struct Response {
    bool success;
    std::string json;

    // Helper to escape strings for JSON
    static std::string escape_json(const std::string& s) {
        std::string result;
        result.reserve(s.size() + 10);
        for (char c : s) {
            switch (c) {
                case '"': result += "\\\""; break;
                case '\\': result += "\\\\"; break;
                case '\n': result += "\\n"; break;
                case '\r': result += "\\r"; break;
                case '\t': result += "\\t"; break;
                default: result += c;
            }
        }
        return result;
    }

    // Success with no payload
    static Response ok() {
        return {true, R"({"ok":true})"};
    }

    // Success with file path
    static Response ok_path(const std::string& path) {
        return {true, R"({"ok":true,"path":")"
            + escape_json(path) + R"("})"};
    }

    // Success with multiple file paths
    static Response ok_paths(const std::vector<std::string>& paths) {
        std::string json = R"({"ok":true,"paths":[)";
        for (size_t i = 0; i < paths.size(); i++) {
            if (i > 0) json += ",";
            json += "\"" + escape_json(paths[i]) + "\"";
        }
        json += R"(],"count":)" + std::to_string(paths.size()) + "}";
        return {true, json};
    }

    // Success with raw JSON data (embedded directly)
    static Response ok_data(const std::string& data) {
        return {true, R"({"ok":true,"data":)" + data + "}"};
    }

    // Error with message
    static Response error(const std::string& msg) {
        return {false, R"({"ok":false,"error":")"
            + escape_json(msg) + R"("})"};
    }

    std::string to_string() const {
        return json + "\n";
    }
};

/**
 * Command handler callback type.
 */
using CommandHandler = std::function<Response(const Command&)>;

/**
 * UNIX domain socket server for control commands.
 */
class ControlSocket {
public:
    explicit ControlSocket(const std::string& socket_path);
    ~ControlSocket();

    // Non-copyable
    ControlSocket(const ControlSocket&) = delete;
    ControlSocket& operator=(const ControlSocket&) = delete;

    /**
     * Start listening for connections.
     */
    bool start();

    /**
     * Stop the server.
     */
    void stop();

    /**
     * Check if server is running.
     */
    bool is_running() const { return running_.load(); }

    /**
     * Register command handlers.
     */
    void set_still_handler(CommandHandler handler) { still_handler_ = handler; }
    void set_burst_handler(CommandHandler handler) { burst_handler_ = handler; }
    void set_clip_handler(CommandHandler handler) { clip_handler_ = handler; }
    void set_set_handler(CommandHandler handler) { set_handler_ = handler; }
    void set_get_handler(CommandHandler handler) { get_handler_ = handler; }

    /**
     * Get number of connected clients.
     */
    size_t client_count() const;

private:
    void accept_thread_func();
    void client_thread_func(int client_fd);
    
    std::optional<Command> parse_command(const std::string& line);
    Response handle_command(const Command& cmd);

    std::string socket_path_;
    int server_fd_ = -1;
    std::atomic<bool> running_{false};

    std::thread accept_thread_;
    
    mutable std::mutex clients_mutex_;
    std::vector<std::thread> client_threads_;
    std::atomic<size_t> client_count_{0};

    CommandHandler still_handler_;
    CommandHandler burst_handler_;
    CommandHandler clip_handler_;
    CommandHandler set_handler_;
    CommandHandler get_handler_;
};

/**
 * Client for connecting to the control socket.
 * Used by consumer applications.
 */
class ControlClient {
public:
    explicit ControlClient(const std::string& socket_path = DEFAULT_SOCKET_PATH);
    ~ControlClient();

    /**
     * Connect to the daemon.
     */
    bool connect();

    /**
     * Disconnect from the daemon.
     */
    void disconnect();

    /**
     * Check if connected.
     */
    bool is_connected() const { return fd_ >= 0; }

    /**
     * Send a command and receive response.
     */
    Response send_command(const std::string& command);

    /**
     * Convenience methods for common commands.
     * @param time_offset_seconds: negative=past, positive=future, 0=now
     */
    Response capture_still(int time_offset_seconds = 0);
    Response capture_burst(int count = 5, int interval_ms = 0);
    Response capture_clip(int start_offset = -5, int end_offset = 5);
    Response set_parameter(const std::string& key, const std::string& value);
    Response get_status();

private:
    std::string socket_path_;
    int fd_ = -1;
};

} // namespace camera_daemon
