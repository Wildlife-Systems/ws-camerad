/**
 * @file ws_camerad_client.hpp
 * @brief C++ client library for ws-camerad camera daemon
 * 
 * This header provides client-side functionality for communicating with
 * the ws-camerad camera daemon via Unix domain socket and shared memory.
 * 
 * Example usage:
 * @code
 *   #include <ws_camerad/client.hpp>
 *   
 *   // Capture a still image
 *   ws_camerad::Client client;
 *   if (client.connect()) {
 *       auto response = client.capture_still();
 *       if (response.ok()) {
 *           std::cout << "Saved to: " << response.path() << std::endl;
 *       }
 *   }
 *   
 *   // Capture a clip (5s before to 5s after now = 10s)
 *   auto response = client.capture_clip(-5, 5);
 *   
 *   // Read frames from shared memory
 *   ws_camerad::FrameReader reader;
 *   if (reader.connect()) {
 *       while (auto frame = reader.read_frame()) {
 *           // process frame->data, frame->width, frame->height
 *       }
 *   }
 * @endcode
 */

#pragma once

#include <string>
#include <optional>
#include <memory>
#include <chrono>
#include <thread>
#include <cstring>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <poll.h>
#include <atomic>

namespace ws_camerad {

/// Default socket path for the camera daemon
constexpr const char* DEFAULT_SOCKET_PATH = "/run/ws-camerad/control.sock";

/// Default frame notification socket path
constexpr const char* DEFAULT_FRAME_NOTIFY_PATH = "/run/ws-camerad/frames.sock";

/// Default shared memory name for raw frames (NV12/YUV)
constexpr const char* DEFAULT_SHM_NAME = "/ws_camera_frames";

/// Default shared memory name for BGR frames (OpenCV-ready)
constexpr const char* DEFAULT_BGR_SHM_NAME = "/ws_camera_frames_bgr";

/**
 * @brief Response from the camera daemon (JSON format)
 * 
 * Responses are JSON:
 *   Success: {"ok":true,"path":"..."} or {"ok":true,"data":{...}}
 *   Error:   {"ok":false,"error":"..."}
 */
class Response {
public:
    Response() = default;
    explicit Response(const std::string& json) : raw_(json) { parse(); }
    
    /// Check if the response indicates success
    bool ok() const { return ok_; }
    
    /// Get the file path (for STILL/CLIP responses)
    const std::string& path() const { return path_; }
    
    /// Get the error message (for failed responses)
    const std::string& error() const { return error_; }
    
    /// Get the raw JSON response
    const std::string& raw() const { return raw_; }
    
    /// Parse a JSON response string
    static Response parse(const std::string& json) {
        return Response(json);
    }

private:
    void parse() {
        ok_ = raw_.find("\"ok\":true") != std::string::npos;
        path_ = extract_string("path");
        error_ = extract_string("error");
    }
    
    std::string extract_string(const std::string& key) const {
        std::string search = "\"" + key + "\":\"";
        auto pos = raw_.find(search);
        if (pos == std::string::npos) return "";
        pos += search.length();
        auto end = raw_.find("\"", pos);
        if (end == std::string::npos) return "";
        
        // Handle escaped characters
        std::string result;
        for (size_t i = pos; i < end; ++i) {
            if (raw_[i] == '\\' && i + 1 < end) {
                ++i;
                switch (raw_[i]) {
                    case 'n': result += '\n'; break;
                    case 'r': result += '\r'; break;
                    case 't': result += '\t'; break;
                    case '"': result += '"'; break;
                    case '\\': result += '\\'; break;
                    default: result += raw_[i];
                }
            } else {
                result += raw_[i];
            }
        }
        return result;
    }
    
    bool ok_ = false;
    std::string path_;
    std::string error_;
    std::string raw_;
};

/**
 * @brief Client for communicating with the camera daemon via control socket
 */
class Client {
public:
    /**
     * @brief Construct a client
     * @param socket_path Path to the daemon's Unix socket
     */
    explicit Client(const std::string& socket_path = DEFAULT_SOCKET_PATH)
        : socket_path_(socket_path) {}
    
    ~Client() { disconnect(); }
    
    // Non-copyable
    Client(const Client&) = delete;
    Client& operator=(const Client&) = delete;
    
    // Movable
    Client(Client&& other) noexcept 
        : socket_path_(std::move(other.socket_path_)), fd_(other.fd_) {
        other.fd_ = -1;
    }
    
    Client& operator=(Client&& other) noexcept {
        if (this != &other) {
            disconnect();
            socket_path_ = std::move(other.socket_path_);
            fd_ = other.fd_;
            other.fd_ = -1;
        }
        return *this;
    }
    
    /**
     * @brief Connect to the camera daemon
     * @return true on success, false on failure
     */
    bool connect() {
        if (fd_ >= 0) return true;  // Already connected
        
        fd_ = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) return false;
        
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
    
    /**
     * @brief Disconnect from the daemon
     */
    void disconnect() {
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
    }
    
    /**
     * @brief Check if connected
     */
    bool is_connected() const { return fd_ >= 0; }
    
    /**
     * @brief Send a command and receive response
     * @param command Command string (without newline)
     * @param timeout_ms Timeout in milliseconds (0 = no timeout)
     * @return Response from daemon
     */
    Response send_command(const std::string& command, int timeout_ms = 30000) {
        if (!connect()) {
            return Response(R"({"ok":false,"error":"Not connected"})");
        }
        
        // Set socket timeout
        if (timeout_ms > 0) {
            struct timeval tv;
            tv.tv_sec = timeout_ms / 1000;
            tv.tv_usec = (timeout_ms % 1000) * 1000;
            setsockopt(fd_, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        }
        
        // Send command
        std::string cmd = command + "\n";
        if (write(fd_, cmd.c_str(), cmd.size()) < 0) {
            return Response(R"({"ok":false,"error":"Write failed"})");
        }
        
        // Read response
        char buffer[8192]{};
        ssize_t n = read(fd_, buffer, sizeof(buffer) - 1);
        if (n <= 0) {
            return Response(R"({"ok":false,"error":"Read failed or timeout"})");
        }
        
        // Trim newline
        while (n > 0 && (buffer[n-1] == '\n' || buffer[n-1] == '\r')) {
            buffer[--n] = '\0';
        }
        
        return Response(buffer);
    }
    
    /**
     * @brief Capture a still image
     * @param time_offset Seconds offset (negative=past, 0=now, positive=future)
     * @return Response with .path() containing image file path
     */
    Response capture_still(int time_offset = 0) {
        if (time_offset == 0) {
            return send_command("STILL");
        }
        return send_command("STILL " + std::to_string(time_offset));
    }
    
    /**
     * @brief Burst capture (multiple stills in rapid succession)
     * @param count Number of images to capture (default 5, max 100)
     * @param interval_ms Milliseconds between captures (0 = as fast as possible)
     * @return Response with JSON containing "paths" array and "count"
     * 
     * Examples:
     *   capture_burst(10)       // 10 images as fast as possible
     *   capture_burst(5, 100)   // 5 images, 100ms apart
     */
    Response capture_burst(int count = 5, int interval_ms = 0) {
        // Timeout scales with count: base 5s + 1s per image
        int timeout_ms = 5000 + count * 1000;
        return send_command("BURST " + std::to_string(count) + " " + 
                           std::to_string(interval_ms), timeout_ms);
    }
    
    /**
     * @brief Capture a video clip
     * @param start_offset Start time relative to now (negative=past, positive=future)
     * @param end_offset End time relative to now (negative=past, positive=future)
     * @return Response with .path() containing clip file path
     * 
     * Examples:
     *   capture_clip(-5, 5)   // 10 seconds: from 5s ago to 5s from now
     *   capture_clip(-10, 0)  // 10 seconds: from 10s ago to now (all buffer)
     *   capture_clip(-3, 2)   // 5 seconds: from 3s ago to 2s from now
     */
    Response capture_clip(int start_offset = -5, int end_offset = 5) {
        // Calculate appropriate timeout: duration + 30s buffer
        int post_event = (end_offset > 0) ? end_offset : 0;
        int timeout_ms = (std::abs(start_offset) + post_event + 30) * 1000;
        
        return send_command("CLIP " + std::to_string(start_offset) + 
                           " " + std::to_string(end_offset), timeout_ms);
    }
    
    /**
     * @brief Get daemon status
     * @return Response with status data
     */
    Response get_status() {
        return send_command("GET STATUS");
    }
    
    /**
     * @brief Set a camera parameter
     * @param key Parameter name
     * @param value Parameter value
     * @return Response indicating success/failure
     */
    Response set_parameter(const std::string& key, const std::string& value) {
        return send_command("SET " + key + " " + value);
    }

private:
    std::string socket_path_;
    int fd_ = -1;
};


/**
 * @brief Shared memory frame header (must match daemon's FramePublisher::Header)
 */
struct FrameHeader {
    std::atomic<uint64_t> sequence;
    std::atomic<uint32_t> frame_ready;
    uint32_t width;
    uint32_t height;
    uint32_t stride;
    uint32_t format;
    uint32_t frame_size;
    uint64_t timestamp_us;
    uint32_t is_keyframe;
    uint32_t reserved[4];
};

/**
 * @brief Frame data returned by FrameReader
 */
struct Frame {
    const uint8_t* data;    ///< Pointer to frame data (read-only)
    uint32_t width;         ///< Frame width in pixels
    uint32_t height;        ///< Frame height in pixels
    uint32_t stride;        ///< Bytes per row
    uint32_t size;          ///< Total frame size in bytes
    uint64_t sequence;      ///< Frame sequence number
    uint64_t timestamp_us;  ///< Capture timestamp in microseconds
    bool is_keyframe;       ///< True if this is a keyframe
};

/**
 * @brief Reader for raw video frames via shared memory
 * 
 * Reads NV12/YUV frames from the default shared memory, or BGR frames
 * from the BGR shared memory (if enabled in daemon config).
 */
class FrameReader {
public:
    /**
     * @brief Construct a frame reader
     * @param shm_name Shared memory name (e.g., DEFAULT_SHM_NAME or DEFAULT_BGR_SHM_NAME)
     */
    explicit FrameReader(const std::string& shm_name = DEFAULT_SHM_NAME)
        : shm_name_(shm_name) {}
    
    ~FrameReader() { disconnect(); }
    
    // Non-copyable, non-movable (due to mmap)
    FrameReader(const FrameReader&) = delete;
    FrameReader& operator=(const FrameReader&) = delete;
    
    /**
     * @brief Connect to shared memory
     * @return true on success
     */
    bool connect() {
        if (mem_) return true;  // Already connected
        
        fd_ = shm_open(shm_name_.c_str(), O_RDONLY, 0);
        if (fd_ < 0) return false;
        
        struct stat sb;
        if (fstat(fd_, &sb) < 0) {
            close(fd_);
            fd_ = -1;
            return false;
        }
        
        mem_size_ = sb.st_size;
        mem_ = mmap(nullptr, mem_size_, PROT_READ, MAP_SHARED, fd_, 0);
        if (mem_ == MAP_FAILED) {
            mem_ = nullptr;
            close(fd_);
            fd_ = -1;
            return false;
        }
        
        header_ = static_cast<const FrameHeader*>(mem_);
        frame_data_ = static_cast<const uint8_t*>(mem_) + sizeof(FrameHeader);
        
        return true;
    }
    
    /**
     * @brief Disconnect from shared memory
     */
    void disconnect() {
        if (mem_) {
            munmap(const_cast<void*>(mem_), mem_size_);
            mem_ = nullptr;
        }
        if (fd_ >= 0) {
            close(fd_);
            fd_ = -1;
        }
        header_ = nullptr;
        frame_data_ = nullptr;
    }
    
    /**
     * @brief Check if connected
     */
    bool is_connected() const { return mem_ != nullptr; }
    
    /**
     * @brief Get frame width
     */
    uint32_t width() const { return header_ ? header_->width : 0; }
    
    /**
     * @brief Get frame height
     */
    uint32_t height() const { return header_ ? header_->height : 0; }
    
    /**
     * @brief Get frame size in bytes
     */
    uint32_t frame_size() const { return header_ ? header_->frame_size : 0; }
    
    /**
     * @brief Set a minimum interval between returned frames (max FPS cap).
     *
     * When set, read_frame() will sleep for the remainder of the interval
     * after the previous frame was returned before polling for a new one.
     * This dramatically reduces syscall count for consumers that don't need
     * every camera frame (e.g. motion detection at 10 fps on a 30 fps stream).
     *
     * @param ms Minimum milliseconds between returned frames (0 = disabled)
     */
    void set_min_interval(int ms) { min_interval_ms_ = ms; }

    /**
     * @brief Read the latest frame
     * @param timeout_ms Timeout in milliseconds (-1 = infinite, 0 = no wait)
     * @return Frame data if available, std::nullopt on timeout
     */
    std::optional<Frame> read_frame(int timeout_ms = 100) {
        if (!header_) return std::nullopt;

        // Rate-limit: sleep for the remainder of the minimum interval
        if (min_interval_ms_ > 0 && last_return_ != std::chrono::steady_clock::time_point{}) {
            auto since = std::chrono::steady_clock::now() - last_return_;
            auto target = std::chrono::milliseconds(min_interval_ms_);
            if (since < target) {
                std::this_thread::sleep_for(target - since);
            }
        }

        auto start = std::chrono::steady_clock::now();
        
        while (true) {
            uint64_t seq = header_->sequence.load(std::memory_order_acquire);
            
            if (seq > last_sequence_ && header_->frame_ready.load(std::memory_order_acquire)) {
                last_sequence_ = seq;
                last_return_ = std::chrono::steady_clock::now();
                
                Frame frame;
                frame.data = frame_data_;
                frame.width = header_->width;
                frame.height = header_->height;
                frame.stride = header_->stride;
                frame.size = header_->frame_size;
                frame.sequence = seq;
                frame.timestamp_us = header_->timestamp_us;
                frame.is_keyframe = header_->is_keyframe != 0;
                
                return frame;
            }
            
            if (timeout_ms == 0) return std::nullopt;
            
            if (timeout_ms > 0) {
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - start);
                if (elapsed.count() >= timeout_ms) return std::nullopt;
            }
            
            usleep(100);  // 100us
        }
    }
    
    /**
     * @brief Check if a new frame is available
     */
    bool has_new_frame() const {
        if (!header_) return false;
        return header_->sequence.load(std::memory_order_acquire) > last_sequence_ &&
               header_->frame_ready.load(std::memory_order_acquire);
    }

private:
    std::string shm_name_;
    int fd_ = -1;
    const void* mem_ = nullptr;
    size_t mem_size_ = 0;
    const FrameHeader* header_ = nullptr;
    const uint8_t* frame_data_ = nullptr;
    uint64_t last_sequence_ = 0;
    int min_interval_ms_ = 0;
    std::chrono::steady_clock::time_point last_return_{};
};

/**
 * @brief Subscribe to frame notifications from the daemon.
 *
 * Instead of polling shared memory in a tight loop, clients can connect to
 * the daemon's frame notification socket and block until a new frame is
 * published. Each notification is an 8-byte uint64_t sequence number.
 *
 * Usage:
 *   FrameNotifySocket notify;
 *   if (notify.connect()) {
 *       while (running) {
 *           uint64_t seq = notify.wait();   // blocks until next frame
 *           auto frame = reader.read_frame(0);  // immediate, frame is ready
 *       }
 *   }
 *
 * This replaces ~45,000 polling syscalls/300 frames with 300 blocking reads.
 */
class FrameNotifySocket {
public:
    /**
     * @param socket_path Path to the daemon's frame notification socket
     */
    explicit FrameNotifySocket(const std::string& socket_path = DEFAULT_FRAME_NOTIFY_PATH)
        : socket_path_(socket_path) {}

    ~FrameNotifySocket() { disconnect(); }

    // Non-copyable
    FrameNotifySocket(const FrameNotifySocket&) = delete;
    FrameNotifySocket& operator=(const FrameNotifySocket&) = delete;

    /**
     * @brief Connect to the daemon's notification socket.
     * @return true on success
     */
    bool connect() {
        if (fd_ >= 0) return true;

        fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd_ < 0) return false;

        struct sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

        if (::connect(fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
            ::close(fd_);
            fd_ = -1;
            return false;
        }
        return true;
    }

    /**
     * @brief Disconnect from the notification socket.
     */
    void disconnect() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    /**
     * @brief Check if connected.
     */
    bool is_connected() const { return fd_ >= 0; }

    /**
     * @brief Block until the next frame is published.
     * @return The sequence number of the new frame, or 0 on error/disconnect.
     */
    uint64_t wait() {
        uint64_t seq = 0;
        ssize_t n = ::read(fd_, &seq, sizeof(seq));
        if (n != sizeof(seq)) {
            // Disconnected or error
            disconnect();
            return 0;
        }
        return seq;
    }

    /**
     * @brief Wait for the next frame with a timeout.
     * @param timeout_ms Timeout in milliseconds
     * @return The sequence number, or 0 on timeout/error.
     */
    uint64_t wait(int timeout_ms) {
        struct pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;
        int ret = ::poll(&pfd, 1, timeout_ms);
        if (ret <= 0) return 0;  // timeout or error
        return wait();
    }

private:
    std::string socket_path_;
    int fd_ = -1;
};

}  // namespace ws_camerad
