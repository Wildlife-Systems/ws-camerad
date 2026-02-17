#include "camera_daemon/frame_notifier.hpp"
#include "camera_daemon/logger.hpp"

#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cerrno>
#include <cstring>
#include <algorithm>

namespace camera_daemon {

// ─── Helpers ────────────────────────────────────────────────────────

static bool set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) >= 0;
}

// ─── FrameNotifier ──────────────────────────────────────────────────

FrameNotifier::FrameNotifier(const std::string& socket_path)
    : socket_path_(socket_path) {}

FrameNotifier::~FrameNotifier() {
    stop();
}

bool FrameNotifier::start() {
    if (running_) return true;

    // Remove stale socket file
    ::unlink(socket_path_.c_str());

    server_fd_ = ::socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        LOG_ERROR("FrameNotifier: socket() failed: ", strerror(errno));
        return false;
    }

    if (!set_nonblocking(server_fd_)) {
        LOG_ERROR("FrameNotifier: failed to set non-blocking: ", strerror(errno));
        ::close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    struct sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::strncpy(addr.sun_path, socket_path_.c_str(), sizeof(addr.sun_path) - 1);

    if (::bind(server_fd_, reinterpret_cast<struct sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("FrameNotifier: bind(", socket_path_, ") failed: ", strerror(errno));
        ::close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    // Allow any user to connect (motion_clip may run as non-root)
    ::chmod(socket_path_.c_str(), 0666);

    if (::listen(server_fd_, 16) < 0) {
        LOG_ERROR("FrameNotifier: listen() failed: ", strerror(errno));
        ::close(server_fd_);
        server_fd_ = -1;
        ::unlink(socket_path_.c_str());
        return false;
    }

    running_ = true;
    accept_thread_ = std::thread(&FrameNotifier::accept_loop, this);

    LOG_INFO("Frame notification socket: ", socket_path_);
    return true;
}

void FrameNotifier::stop() {
    if (!running_.exchange(false)) return;

    // Shut down server fd to wake the accept loop
    if (server_fd_ >= 0) {
        ::shutdown(server_fd_, SHUT_RDWR);
        ::close(server_fd_);
        server_fd_ = -1;
    }

    if (accept_thread_.joinable()) {
        accept_thread_.join();
    }

    // Close all client connections
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (int fd : client_fds_) {
            ::close(fd);
        }
        client_fds_.clear();
    }

    ::unlink(socket_path_.c_str());
}

void FrameNotifier::notify(uint64_t sequence) {
    // Fast path: no subscribers — just return
    std::lock_guard<std::mutex> lock(clients_mutex_);
    if (client_fds_.empty()) return;

    // Write the 8-byte sequence number to every client (non-blocking).
    // If a client can't keep up (EAGAIN) or has disconnected (EPIPE),
    // close it and remove from the list.
    auto it = client_fds_.begin();
    while (it != client_fds_.end()) {
        ssize_t n = ::send(*it, &sequence, sizeof(sequence), MSG_DONTWAIT | MSG_NOSIGNAL);
        if (n != sizeof(sequence)) {
            // Client is dead or can't keep up — drop it
            ::close(*it);
            it = client_fds_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t FrameNotifier::subscriber_count() const {
    std::lock_guard<std::mutex> lock(clients_mutex_);
    return client_fds_.size();
}

void FrameNotifier::accept_loop() {
    struct pollfd pfd{};
    pfd.fd = server_fd_;
    pfd.events = POLLIN;

    while (running_) {
        int ret = ::poll(&pfd, 1, 200);  // 200ms timeout
        if (ret <= 0) continue;

        int client_fd = ::accept(server_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) continue;
            if (!running_) break;
            LOG_ERROR("FrameNotifier: accept() failed: ", strerror(errno));
            continue;
        }

        // Set client fd to non-blocking for writes
        set_nonblocking(client_fd);

        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            client_fds_.push_back(client_fd);
        }

        LOG_DEBUG("Frame notification subscriber connected (fd=", client_fd, ")");
    }
}

} // namespace camera_daemon
