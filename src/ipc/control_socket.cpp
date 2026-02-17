#include "camera_daemon/control_socket.hpp"
#include "camera_daemon/logger.hpp"
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <algorithm>
#include <sstream>
#include <optional>

namespace camera_daemon {

ControlSocket::ControlSocket(const std::string& socket_path)
    : socket_path_(socket_path) {
}

ControlSocket::~ControlSocket() {
    stop();
}

bool ControlSocket::start() {
    if (running_) {
        return true;
    }

    // Remove existing socket file
    unlink(socket_path_.c_str());

    // Create socket
    server_fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        LOG_ERROR("Failed to create socket: ", strerror(errno));
        return false;
    }

    // Set non-blocking
    int flags = fcntl(server_fd_, F_GETFL, 0);
    fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);

    // Bind
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind socket: ", strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    // Listen
    if (listen(server_fd_, 5) < 0) {
        LOG_ERROR("Failed to listen on socket: ", strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    // Set permissions (readable/writable by all users)
    chmod(socket_path_.c_str(), S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH | S_IWOTH);

    running_ = true;
    accept_thread_ = std::thread(&ControlSocket::accept_thread_func, this);

    LOG_INFO("Control socket listening on: ", socket_path_);
    return true;
}

void ControlSocket::stop() {
    if (!running_) {
        return;
    }

    LOG_INFO("Stopping control socket");
    running_ = false;

    if (server_fd_ >= 0) {
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    // Wait for client threads
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (auto& thread : client_threads_) {
            if (thread.joinable()) {
                thread.join();
            }
        }
        client_threads_.clear();
    }

    unlink(socket_path_.c_str());
    LOG_INFO("Control socket stopped");
}

void ControlSocket::accept_thread_func() {
    LOG_DEBUG("Accept thread started");

    while (running_) {
        pollfd pfd{};
        pfd.fd = server_fd_;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 100);  // 100ms timeout
        if (ret <= 0 || !running_) {
            continue;
        }

        int client_fd = accept(server_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR("Accept failed: ", strerror(errno));
            }
            continue;
        }

        LOG_DEBUG("Client connected, fd=", client_fd);
        client_count_++;

        std::lock_guard<std::mutex> lock(clients_mutex_);
        client_threads_.emplace_back(&ControlSocket::client_thread_func, this, client_fd);
    }

    LOG_DEBUG("Accept thread stopped");
}

void ControlSocket::client_thread_func(int client_fd) {
    char buffer[4096];
    std::string line_buffer;

    while (running_) {
        pollfd pfd{};
        pfd.fd = client_fd;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 100);
        if (ret < 0) {
            break;
        }
        if (ret == 0) {
            continue;  // Timeout, check if still running
        }

        ssize_t n = read(client_fd, buffer, sizeof(buffer) - 1);
        if (n <= 0) {
            break;  // Connection closed or error
        }

        buffer[n] = '\0';
        line_buffer += buffer;

        // Process complete lines
        size_t pos;
        while ((pos = line_buffer.find('\n')) != std::string::npos) {
            std::string line = line_buffer.substr(0, pos);
            line_buffer = line_buffer.substr(pos + 1);

            // Remove carriage return if present
            if (!line.empty() && line.back() == '\r') {
                line.pop_back();
            }

            if (line.empty()) {
                continue;
            }

            LOG_DEBUG("Received command: ", line);

            auto cmd = parse_command(line);
            Response response;
            
            if (cmd) {
                response = handle_command(*cmd);
            } else {
                response = Response::error("Invalid command");
            }

            std::string response_str = response.to_string();
            ssize_t written = write(client_fd, response_str.c_str(), response_str.size());
            if (written < 0) {
                LOG_ERROR("Failed to write response to client: ", strerror(errno));
                break;
            }
        }
    }

    close(client_fd);
    client_count_--;
    LOG_DEBUG("Client disconnected");
}

std::optional<Command> ControlSocket::parse_command(const std::string& line) {
    std::istringstream iss(line);
    std::string cmd_str;
    iss >> cmd_str;

    // Convert to uppercase
    std::transform(cmd_str.begin(), cmd_str.end(), cmd_str.begin(), ::toupper);

    Command cmd;

    if (cmd_str == "STILL") {
        cmd.type = CommandType::STILL;
        // First param: negative = n seconds ago, positive = in n seconds, 0 = now
        iss >> cmd.int_value;
        // int_value defaults to 0 if not provided
    }
    else if (cmd_str == "BURST") {
        cmd.type = CommandType::BURST;
        // First param: count (default 5)
        // Second param: interval_ms (default 0 = as fast as possible)
        iss >> cmd.int_value >> cmd.int_value2;
        if (cmd.int_value <= 0) {
            cmd.int_value = 5;  // Default: 5 images
        }
        // int_value2 defaults to 0 (no delay)
    }
    else if (cmd_str == "CLIP") {
        cmd.type = CommandType::CLIP;
        // First param: start offset (negative=past, positive=future)
        // Second param: end offset (negative=past, positive=future)
        // e.g., CLIP -5 5 = 10 seconds (from 5s ago to 5s from now)
        iss >> cmd.int_value >> cmd.int_value2;
        // Defaults: -5 to 5 (10 second clip centered on now)
        if (cmd.int_value == 0 && cmd.int_value2 == 0) {
            cmd.int_value = -5;
            cmd.int_value2 = 5;
        }
    }
    else if (cmd_str == "SET") {
        cmd.type = CommandType::SET;
        iss >> cmd.key >> cmd.value;
        if (cmd.key.empty()) {
            return std::nullopt;
        }
    }
    else if (cmd_str == "GET") {
        cmd.type = CommandType::GET;
        iss >> cmd.key;
        if (cmd.key.empty()) {
            cmd.key = "STATUS";
        }
        std::transform(cmd.key.begin(), cmd.key.end(), cmd.key.begin(), ::toupper);
    }
    else if (cmd_str == "QUIT" || cmd_str == "EXIT") {
        cmd.type = CommandType::QUIT;
    }
    else {
        return std::nullopt;
    }

    return cmd;
}

Response ControlSocket::handle_command(const Command& cmd) {
    switch (cmd.type) {
        case CommandType::STILL:
            if (still_handler_) {
                return still_handler_(cmd);
            }
            return Response::error("Still capture not configured");

        case CommandType::BURST:
            if (burst_handler_) {
                return burst_handler_(cmd);
            }
            return Response::error("Burst capture not configured");

        case CommandType::CLIP:
            if (clip_handler_) {
                return clip_handler_(cmd);
            }
            return Response::error("Clip extraction not configured");

        case CommandType::SET:
            if (set_handler_) {
                return set_handler_(cmd);
            }
            return Response::error("Set handler not configured");

        case CommandType::GET:
            if (get_handler_) {
                return get_handler_(cmd);
            }
            return Response::error("Get handler not configured");

        case CommandType::QUIT:
            return Response::ok();

        default:
            return Response::error("Unknown command");
    }
}

size_t ControlSocket::client_count() const {
    return client_count_.load();
}

// ControlClient implementation

ControlClient::ControlClient(const std::string& socket_path)
    : socket_path_(socket_path) {
}

ControlClient::~ControlClient() {
    disconnect();
}

bool ControlClient::connect() {
    if (fd_ >= 0) {
        return true;
    }

    fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd_ < 0) {
        return false;
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::connect(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        close(fd_);
        fd_ = -1;
        return false;
    }

    return true;
}

void ControlClient::disconnect() {
    if (fd_ >= 0) {
        close(fd_);
        fd_ = -1;
    }
}

Response ControlClient::send_command(const std::string& command) {
    if (!is_connected() && !connect()) {
        return Response::error("Not connected");
    }

    std::string cmd = command;
    if (cmd.empty() || cmd.back() != '\n') {
        cmd += '\n';
    }

    if (write(fd_, cmd.c_str(), cmd.size()) < 0) {
        return Response::error("Write failed");
    }

    char buffer[4096];
    ssize_t n = read(fd_, buffer, sizeof(buffer) - 1);
    if (n <= 0) {
        return Response::error("Read failed");
    }

    buffer[n] = '\0';
    std::string response(buffer);
    
    // Remove trailing newline
    while (!response.empty() && (response.back() == '\n' || response.back() == '\r')) {
        response.pop_back();
    }

    // Parse JSON response - look for "ok":true or "ok":false
    bool success = response.find("\"ok\":true") != std::string::npos;
    return {success, response};
}

Response ControlClient::capture_still(int time_offset_seconds) {
    if (time_offset_seconds == 0) {
        return send_command("STILL");
    }
    return send_command("STILL " + std::to_string(time_offset_seconds));
}

Response ControlClient::capture_burst(int count, int interval_ms) {
    return send_command("BURST " + std::to_string(count) + " " + std::to_string(interval_ms));
}

Response ControlClient::capture_clip(int start_offset, int end_offset) {
    return send_command("CLIP " + std::to_string(start_offset) + " " + std::to_string(end_offset));
}

Response ControlClient::set_parameter(const std::string& key, const std::string& value) {
    return send_command("SET " + key + " " + value);
}

Response ControlClient::get_status() {
    return send_command("GET STATUS");
}

} // namespace camera_daemon
