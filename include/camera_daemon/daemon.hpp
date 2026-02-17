#pragma once

#include "common.hpp"
#include "capture_pipeline.hpp"
#include "control_socket.hpp"
#include <memory>
#include <atomic>
#include <csignal>

namespace camera_daemon {

/**
 * Main daemon class.
 */
class Daemon {
public:
    explicit Daemon(const DaemonConfig& config);
    ~Daemon();

    // Non-copyable
    Daemon(const Daemon&) = delete;
    Daemon& operator=(const Daemon&) = delete;

    /**
     * Initialize all daemon components.
     */
    bool initialize();

    /**
     * Run the daemon (blocking).
     * Returns when stop() is called or a signal is received.
     */
    int run();

    /**
     * Stop the daemon.
     */
    void stop();

    /**
     * Signal handler for graceful shutdown.
     */
    static void signal_handler(int signum);

    /**
     * Get the singleton instance (for signal handling).
     */
    static Daemon* instance() { return instance_; }

private:
    Response handle_still_command(const Command& cmd);
    Response handle_burst_command(const Command& cmd);
    Response handle_clip_command(const Command& cmd);
    Response handle_set_command(const Command& cmd);
    Response handle_get_command(const Command& cmd);

    DaemonConfig config_;
    std::unique_ptr<CapturePipeline> pipeline_;
    std::unique_ptr<ControlSocket> control_socket_;

    std::atomic<bool> running_{false};

    static Daemon* instance_;
};

/**
 * Parse command line arguments.
 */
DaemonConfig parse_args(int argc, char* argv[]);

/**
 * Load configuration from file.
 */
DaemonConfig load_config(const std::string& path);

} // namespace camera_daemon
