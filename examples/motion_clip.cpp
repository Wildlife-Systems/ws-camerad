/**
 * Motion-Triggered Clip Capture (Optimized)
 *
 * Reads raw YUV frames from ws-camerad shared memory, extracts the Y (luma)
 * plane directly as grayscale, downsamples 4x, and runs motion detection.
 * Triggers a 4-second clip (-2s to +2s) on each event with a 2-second cooldown.
 *
 * Optimizations:
 *   1. Uses raw YUV shm — Y plane IS grayscale, no BGR conversion needed
 *   2. Downsamples 4x before processing (e.g. 320x240 vs 1280x960)
 *   3. Skips frames — analyzes every Nth frame (default: 3)
 *   4. Pre-allocated buffers — no per-frame heap allocation
 *   5. Clip capture in detached thread — non-blocking
 *
 * Requirements:
 *   sudo apt install libopencv-dev
 *
 * Usage:
 *   ./motion_clip
 *   ./motion_clip --threshold 30 --min-area 800
 *   ./motion_clip --pre -3 --post 3 --cooldown 5 --skip 5
 */

#if __has_include(<ws_camerad/client.hpp>)
#include <ws_camerad/client.hpp>
#else
#include "../include/ws_camerad/client.hpp"
#endif

#include <opencv2/imgproc.hpp>
#include <iostream>
#include <chrono>
#include <csignal>
#include <cstring>
#include <thread>
#include <atomic>
#include <getopt.h>

static volatile bool g_running = true;

static void signal_handler(int) {
    g_running = false;
}

/**
 * Motion detector with pre-allocated buffers and downsampling.
 * Operates on single-channel (grayscale/Y-plane) input.
 */
class MotionDetector {
public:
    MotionDetector(double threshold, int min_area, int scale_down)
        : threshold_(threshold), min_area_(min_area), scale_(scale_down) {}

    /**
     * Initialise pre-allocated buffers once dimensions are known.
     */
    void init(uint32_t full_w, uint32_t full_h) {
        small_w_ = full_w / scale_;
        small_h_ = full_h / scale_;
        // Scale min_area for the smaller image
        scaled_min_area_ = min_area_ / (scale_ * scale_);
        if (scaled_min_area_ < 1) scaled_min_area_ = 1;

        small_.create(small_h_, small_w_, CV_8UC1);
        blurred_.create(small_h_, small_w_, CV_8UC1);
        diff_.create(small_h_, small_w_, CV_8UC1);
        thresh_.create(small_h_, small_w_, CV_8UC1);
        prev_blurred_.release();  // will be set on first frame
    }

    /**
     * Detect motion from a full-resolution grayscale (Y-plane) Mat.
     */
    bool detect(const cv::Mat& gray_full) {
        // Downsample
        cv::resize(gray_full, small_, cv::Size(small_w_, small_h_), 0, 0, cv::INTER_NEAREST);
        cv::GaussianBlur(small_, blurred_, cv::Size(21, 21), 0);

        if (prev_blurred_.empty()) {
            blurred_.copyTo(prev_blurred_);
            return false;
        }

        cv::absdiff(prev_blurred_, blurred_, diff_);
        cv::threshold(diff_, thresh_, threshold_, 255, cv::THRESH_BINARY);
        cv::dilate(thresh_, thresh_, cv::Mat(), cv::Point(-1, -1), 2);

        cv::findContours(thresh_, contours_, cv::RETR_EXTERNAL, cv::CHAIN_APPROX_SIMPLE);

        bool motion = false;
        for (const auto& c : contours_) {
            if (cv::contourArea(c) >= scaled_min_area_) {
                motion = true;
                break;
            }
        }

        blurred_.copyTo(prev_blurred_);
        return motion;
    }

private:
    double threshold_;
    int min_area_;
    int scale_;
    int small_w_ = 0, small_h_ = 0;
    double scaled_min_area_ = 0;

    // Pre-allocated buffers
    cv::Mat small_, blurred_, diff_, thresh_, prev_blurred_;
    std::vector<std::vector<cv::Point>> contours_;
};

static std::string timestamp_str() {
    auto now = std::chrono::system_clock::now();
    auto t   = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
    localtime_r(&t, &tm);
    char buf[16];
    std::strftime(buf, sizeof(buf), "%H:%M:%S", &tm);
    return buf;
}

/**
 * Capture a clip in a detached thread so detection keeps running.
 */
static std::atomic<bool> g_clip_in_progress{false};

static void capture_clip_async(int pre, int post) {
    ws_camerad::Client client;
    if (!client.connect()) {
        std::cout << "failed to connect to daemon" << std::endl;
        g_clip_in_progress.store(false, std::memory_order_release);
        return;
    }

    auto resp = client.capture_clip(pre, post);
    if (resp.ok()) {
        std::cout << "saved: " << resp.path() << std::endl;
    } else {
        std::cout << "clip capture failed: " << resp.error() << std::endl;
    }
    g_clip_in_progress.store(false, std::memory_order_release);
}

static void print_usage(const char* prog) {
    std::cout << "Usage: " << prog << " [OPTIONS]\n"
              << "Options:\n"
              << "  --shm NAME        Raw YUV shared memory name (default: "
              << ws_camerad::DEFAULT_SHM_NAME << ")\n"
              << "  --threshold VAL   Motion threshold 0-255 (default: 25)\n"
              << "  --min-area VAL    Min contour area in pixels (default: 500)\n"
              << "  --cooldown SECS   Seconds between triggers (default: 2.0)\n"
              << "  --pre SECS        Seconds before event (default: -2)\n"
              << "  --post SECS       Seconds after event (default: 2)\n"
              << "  --skip N          Analyze every Nth frame (default: 3)\n"
              << "  --scale N         Downsample factor (default: 4)\n"
              << "  -h, --help        Show this help\n";
}

int main(int argc, char* argv[]) {
    // Defaults
    // Default matches daemon config shm_name (/ws_camerad_frames)
    std::string shm_name = "/ws_camerad_frames";
    double threshold  = 25.0;
    int    min_area   = 500;
    double cooldown   = 2.0;
    int    pre        = -2;
    int    post       = 2;
    int    skip       = 3;
    int    scale      = 4;

    // Parse options
    static struct option long_opts[] = {
        {"shm",       required_argument, nullptr, 's'},
        {"threshold", required_argument, nullptr, 't'},
        {"min-area",  required_argument, nullptr, 'a'},
        {"cooldown",  required_argument, nullptr, 'c'},
        {"pre",       required_argument, nullptr, 'p'},
        {"post",      required_argument, nullptr, 'P'},
        {"skip",      required_argument, nullptr, 'n'},
        {"scale",     required_argument, nullptr, 'd'},
        {"help",      no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "hs:t:a:c:p:P:n:d:", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 's': shm_name  = optarg;              break;
            case 't': threshold = std::stod(optarg);   break;
            case 'a': min_area  = std::stoi(optarg);   break;
            case 'c': cooldown  = std::stod(optarg);   break;
            case 'p': pre       = std::stoi(optarg);   break;
            case 'P': post      = std::stoi(optarg);   break;
            case 'n': skip      = std::stoi(optarg);   break;
            case 'd': scale     = std::stoi(optarg);   break;
            case 'h':
            default:
                print_usage(argv[0]);
                return (opt == 'h') ? 0 : 1;
        }
    }
    if (skip < 1) skip = 1;
    if (scale < 1) scale = 1;

    // Signal handling
    std::signal(SIGINT,  signal_handler);
    std::signal(SIGTERM, signal_handler);

    // Connect to raw YUV shared memory (Y plane = grayscale)
    ws_camerad::FrameReader reader(shm_name);
    if (!reader.connect()) {
        std::cerr << "Failed to connect to shared memory (" << shm_name << ").\n"
                  << "Is ws-camerad running with enable_raw_sharing=true?" << std::endl;
        return 1;
    }

    const uint32_t w = reader.width();
    const uint32_t h = reader.height();

    MotionDetector detector(threshold, min_area, scale);
    detector.init(w, h);

    std::cout << "Connected: " << w << "x" << h
              << " (processing at " << w/scale << "x" << h/scale << ")\n"
              << "Motion detection active (threshold=" << threshold
              << ", min_area=" << min_area << ")\n"
              << "Frame skip: every " << skip << " frame(s), downsample: " << scale << "x\n"
              << "Clip window: " << pre << "s to +" << post << "s ("
              << (post - pre) << "s total)\n"
              << "Cooldown: " << cooldown << "s\n"
              << "Watching for motion... Press Ctrl+C to stop.\n" << std::endl;

    using clock = std::chrono::steady_clock;
    auto last_trigger = clock::time_point{};
    uint64_t frame_count = 0;

    while (g_running) {
        auto frame = reader.read_frame(100);
        if (!frame) continue;

        // Skip frames: only analyze every Nth
        if (++frame_count % skip != 0) continue;

        // Wrap the Y plane directly as grayscale (first w*h bytes of NV12/YUV420)
        cv::Mat y_plane(h, w, CV_8UC1, const_cast<uint8_t*>(frame->data), frame->stride);

        if (!detector.detect(y_plane)) continue;

        // Cooldown check
        auto now = clock::now();
        double since = std::chrono::duration<double>(now - last_trigger).count();
        if (since < cooldown) continue;

        // Don't trigger if a clip is already being captured
        if (g_clip_in_progress.load(std::memory_order_acquire)) continue;

        last_trigger = now;
        g_clip_in_progress.store(true, std::memory_order_release);

        std::cout << "[" << timestamp_str() << "] Motion detected — capturing clip... "
                  << std::flush;

        // Fire clip capture in a detached thread
        std::thread(capture_clip_async, pre, post).detach();
    }

    // Wait for any in-flight clip to finish
    while (g_clip_in_progress.load(std::memory_order_acquire)) {
        usleep(100000);
    }

    std::cout << "\nStopping." << std::endl;
    return 0;
}
