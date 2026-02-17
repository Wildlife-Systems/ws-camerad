#pragma once

#include "common.hpp"
#include <string>
#include <thread>
#include <atomic>
#include <mutex>
#include <vector>
#include <deque>
#include <condition_variable>

namespace camera_daemon {

/**
 * RTSP server for remote video streaming.
 * Read-only access to the encoded video stream.
 */
class RTSPServer {
public:
    struct Config {
        uint16_t port = 8554;
        std::string mount_point = "/camera";
        uint32_t width;
        uint32_t height;
        uint32_t framerate;
        bool h265 = false;
    };

    explicit RTSPServer(const Config& config);
    ~RTSPServer();

    // Non-copyable
    RTSPServer(const RTSPServer&) = delete;
    RTSPServer& operator=(const RTSPServer&) = delete;

    /**
     * Start the RTSP server.
     */
    bool start();

    /**
     * Stop the RTSP server.
     */
    void stop();

    /**
     * Check if server is running.
     */
    bool is_running() const { return running_.load(); }

    /**
     * Push an encoded frame to connected clients.
     */
    void push_frame(const EncodedFrame& frame);

    /**
     * Get the RTSP URL for this stream.
     */
    std::string get_url() const;

    /**
     * Get number of connected clients.
     */
    size_t client_count() const;

    /**
     * Get statistics.
     */
    struct Stats {
        uint64_t frames_sent;
        uint64_t bytes_sent;
        size_t connected_clients;
    };
    Stats get_stats() const;

private:
#ifdef HAVE_GSTREAMER
    void gst_thread_func();
    
    struct GstData;
    std::unique_ptr<GstData> gst_data_;
    std::thread gst_thread_;
#else
    // Simple TCP-based fallback
    void server_thread_func();
    void client_thread_func(int client_fd);
    
    int server_fd_ = -1;
    std::thread server_thread_;
    
    mutable std::mutex clients_mutex_;
    std::vector<int> client_fds_;
    std::vector<std::thread> client_threads_;
#endif

    Config config_;
    std::atomic<bool> running_{false};

    // Frame queue for streaming (deque for O(1) pop_front)
    mutable std::mutex frame_mutex_;
    std::condition_variable frame_cv_;
    std::deque<EncodedFrame> frame_queue_;
    static constexpr size_t MAX_QUEUE_SIZE = 30;

    // Statistics
    std::atomic<uint64_t> frames_sent_{0};
    std::atomic<uint64_t> bytes_sent_{0};
    std::atomic<size_t> client_count_{0};
};

} // namespace camera_daemon
