#pragma once

#include "common.hpp"
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>

namespace camera_daemon {

/**
 * Unix domain socket server that notifies subscribed clients when a new
 * frame has been published to shared memory.
 *
 * Protocol (per notification):
 *   8 bytes: uint64_t sequence number (native byte order)
 *
 * Clients simply connect to the socket and block on read(). Each 8-byte
 * message means "a new frame with this sequence number is now available
 * in shared memory." This replaces polling with a single blocking read
 * per frame.
 *
 * Design notes:
 *   - Non-blocking writes to clients (slow/dead clients are dropped)
 *   - Thread-safe: notify() can be called from the camera frame callback
 *   - Zero allocation in the hot path (notify)
 */
class FrameNotifier {
public:
    /**
     * @param socket_path  Path for the Unix domain socket
     *                     (default: /run/ws-camerad/frames.sock)
     */
    explicit FrameNotifier(const std::string& socket_path = "/run/ws-camerad/frames.sock");
    ~FrameNotifier();

    // Non-copyable
    FrameNotifier(const FrameNotifier&) = delete;
    FrameNotifier& operator=(const FrameNotifier&) = delete;

    /**
     * Start listening for subscriber connections.
     * @return true on success
     */
    bool start();

    /**
     * Stop the server and disconnect all clients.
     */
    void stop();

    /**
     * Notify all connected clients that a new frame is available.
     * Called from the camera frame callback — must be fast and lock-free
     * on the happy path (no new connections/disconnections).
     *
     * @param sequence  Frame sequence number (matches shm header)
     */
    void notify(uint64_t sequence);

    /**
     * Get number of connected subscribers.
     */
    size_t subscriber_count() const;

    /**
     * Check if running.
     */
    bool is_running() const { return running_.load(std::memory_order_relaxed); }

private:
    void accept_loop();

    std::string socket_path_;
    int server_fd_ = -1;
    std::atomic<bool> running_{false};
    std::thread accept_thread_;

    mutable std::mutex clients_mutex_;
    std::vector<int> client_fds_;
};

} // namespace camera_daemon
