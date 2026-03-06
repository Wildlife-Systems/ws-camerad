/**
 * Motion detection benchmark — measures per-frame detection time.
 *
 * Modes:
 *   Mode A (default): Raw YUV Y-plane shm → downsample 4x → detect (C++ native path)
 *   Mode B (--bgr):   BGR shared memory → cvtColor(BGR2GRAY) → downsample 4x → detect
 *   Mode C (--v4l2 N): V4L2 virtual camera (/dev/videoN) → cvtColor → detect
 *                      If daemon outputs pre-downsampled frames, use --no-downsample
 *
 * Usage:
 *   ./bench_motion              # Mode A: raw YUV path
 *   ./bench_motion --bgr        # Mode B: BGR path (matches Python pipeline)
 *   ./bench_motion --v4l2 30 --no-downsample  # Mode C: virtual camera (pre-downsampled)
 */

#if __has_include(<ws_camerad/client.hpp>)
#include <ws_camerad/client.hpp>
#else
#include "../include/ws_camerad/client.hpp"
#endif

#include <opencv2/imgproc.hpp>
#include <opencv2/videoio.hpp>
#include <iostream>
#include <chrono>
#include <vector>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <csignal>
#include <getopt.h>

static volatile bool g_running = true;
static void signal_handler(int) { g_running = false; }

class MotionDetector {
public:
    MotionDetector(double threshold, int min_area, int scale_down)
        : threshold_(threshold), min_area_(min_area), scale_(scale_down) {}

    void init(uint32_t full_w, uint32_t full_h) {
        small_w_ = full_w / scale_;
        small_h_ = full_h / scale_;
        scaled_min_area_ = min_area_ / (scale_ * scale_);
        if (scaled_min_area_ < 1) scaled_min_area_ = 1;
        small_.create(small_h_, small_w_, CV_8UC1);
        blurred_.create(small_h_, small_w_, CV_8UC1);
        diff_.create(small_h_, small_w_, CV_8UC1);
        thresh_.create(small_h_, small_w_, CV_8UC1);
        prev_blurred_.release();
    }

    bool detect(const cv::Mat& gray_full) {
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
            if (cv::contourArea(c) >= scaled_min_area_) { motion = true; break; }
        }
        blurred_.copyTo(prev_blurred_);
        return motion;
    }

private:
    double threshold_;
    int min_area_, scale_;
    int small_w_ = 0, small_h_ = 0;
    double scaled_min_area_ = 0;
    cv::Mat small_, blurred_, diff_, thresh_, prev_blurred_;
    std::vector<std::vector<cv::Point>> contours_;
};

struct Stats {
    double mean_us, median_us, p95_us, p99_us, min_us, max_us, stddev_us;
    int motion_frames;
    int total_frames;
};

Stats compute_stats(std::vector<double>& times, int motion_count) {
    Stats s{};
    s.total_frames = times.size();
    s.motion_frames = motion_count;
    if (times.empty()) return s;

    std::sort(times.begin(), times.end());
    double sum = std::accumulate(times.begin(), times.end(), 0.0);
    s.mean_us = sum / times.size();
    s.median_us = times[times.size() / 2];
    s.p95_us = times[static_cast<size_t>(times.size() * 0.95)];
    s.p99_us = times[static_cast<size_t>(times.size() * 0.99)];
    s.min_us = times.front();
    s.max_us = times.back();

    double sq_sum = 0;
    for (auto t : times) sq_sum += (t - s.mean_us) * (t - s.mean_us);
    s.stddev_us = std::sqrt(sq_sum / times.size());

    return s;
}

int main(int argc, char* argv[]) {
    bool use_bgr = false;
    bool downsample = true;
    int v4l2_dev = -1;  // -1 = disabled, else /dev/videoN
    int num_frames = 300;  // ~10s at 30fps
    double threshold = 25.0;
    int min_area = 500;
    int scale = 4;
    double max_fps = 0;  // 0 = unlimited

    static struct option long_opts[] = {
        {"bgr",           no_argument,       nullptr, 'b'},
        {"v4l2",          required_argument, nullptr, 'v'},
        {"no-downsample", no_argument,       nullptr, 'D'},
        {"frames",        required_argument, nullptr, 'f'},
        {"threshold",     required_argument, nullptr, 't'},
        {"min-area",      required_argument, nullptr, 'a'},
        {"scale",         required_argument, nullptr, 's'},
        {"max-fps",       required_argument, nullptr, 'r'},
        {"help",          no_argument,       nullptr, 'h'},
        {nullptr, 0, nullptr, 0}
    };

    int opt;
    while ((opt = getopt_long(argc, argv, "bv:Df:t:a:s:r:h", long_opts, nullptr)) != -1) {
        switch (opt) {
            case 'b': use_bgr = true; break;
            case 'v': v4l2_dev = std::stoi(optarg); break;
            case 'D': downsample = false; break;
            case 'f': num_frames = std::stoi(optarg); break;
            case 't': threshold = std::stod(optarg); break;
            case 'a': min_area = std::stoi(optarg); break;
            case 's': scale = std::stoi(optarg); break;
            case 'r': max_fps = std::stod(optarg); break;
            case 'h':
            default:
                std::cout << "Usage: " << argv[0]
                          << " [--bgr] [--v4l2 N] [--no-downsample] [--frames N]\n";
                return (opt == 'h') ? 0 : 1;
        }
    }

    if (!downsample) scale = 1;

    int min_interval_ms = 0;
    if (max_fps > 0) {
        min_interval_ms = static_cast<int>(std::round(1000.0 / max_fps));
    }

    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    uint32_t w, h;
    std::vector<double> read_times, detect_times, total_times;
    read_times.reserve(num_frames);
    detect_times.reserve(num_frames);
    total_times.reserve(num_frames);
    int motion_count = 0;

    if (v4l2_dev >= 0) {
        // Mode C: V4L2 virtual camera (daemon-side downsampled)
        std::string dev = "/dev/video" + std::to_string(v4l2_dev);
        cv::VideoCapture cap(dev, cv::CAP_V4L2);
        if (!cap.isOpened()) {
            std::cerr << "Failed to open V4L2 device: " << dev << "\n";
            return 1;
        }

        w = static_cast<uint32_t>(cap.get(cv::CAP_PROP_FRAME_WIDTH));
        h = static_cast<uint32_t>(cap.get(cv::CAP_PROP_FRAME_HEIGHT));

        MotionDetector detector(threshold, min_area, scale);
        detector.init(w, h);

        std::cout << "=== C++ Motion Benchmark (V4L2 virtual camera: " << dev << ") ===\n"
                  << "Resolution: " << w << "x" << h
                  << " → " << w/scale << "x" << h/scale << "\n"
                  << "Frames: " << num_frames << "\n"
                  << "Threshold: " << threshold << ", MinArea: " << min_area << "\n"
                  << "Scale: " << scale << "x\n\n";

        using clock = std::chrono::high_resolution_clock;
        cv::Mat frame_bgr, gray;

        for (int i = 0; i < num_frames && g_running; ) {
            auto t0 = clock::now();
            if (!cap.read(frame_bgr) || frame_bgr.empty()) continue;
            auto t1 = clock::now();

            cv::cvtColor(frame_bgr, gray, cv::COLOR_BGR2GRAY);
            bool motion = detector.detect(gray);
            auto t2 = clock::now();

            if (motion) motion_count++;

            double read_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            double det_us = std::chrono::duration<double, std::micro>(t2 - t1).count();
            double tot_us = std::chrono::duration<double, std::micro>(t2 - t0).count();

            read_times.push_back(read_us);
            detect_times.push_back(det_us);
            total_times.push_back(tot_us);
            i++;
        }
        cap.release();
    } else if (use_bgr) {
        // Mode B: BGR path (equivalent to Python)
        ws_camerad::FrameReader reader(ws_camerad::DEFAULT_BGR_SHM_NAME);
        if (!reader.connect()) {
            std::cerr << "Failed to connect to BGR shared memory.\n";
            return 1;
        }
        if (min_interval_ms > 0) reader.set_min_interval(min_interval_ms);
        w = reader.width();
        h = reader.height();

        MotionDetector detector(threshold, min_area, scale);
        detector.init(w, h);

        std::cout << "=== C++ Motion Benchmark (BGR path) ===\n"
                  << "Resolution: " << w << "x" << h
                  << " → " << w/scale << "x" << h/scale << "\n"
                  << "Frames: " << num_frames << "\n"
                  << "Threshold: " << threshold << ", MinArea: " << min_area << "\n"
                  << "Scale: " << scale << "x\n\n";

        using clock = std::chrono::high_resolution_clock;

        for (int i = 0; i < num_frames && g_running; ) {
            auto t0 = clock::now();
            auto frame = reader.read_frame(100);
            if (!frame) continue;
            auto t1 = clock::now();

            // BGR → grayscale (same as Python cv2.cvtColor)
            cv::Mat bgr(h, w, CV_8UC3, const_cast<uint8_t*>(frame->data));
            cv::Mat gray;
            cv::cvtColor(bgr, gray, cv::COLOR_BGR2GRAY);

            bool motion = detector.detect(gray);
            auto t2 = clock::now();

            if (motion) motion_count++;

            double read_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            double det_us = std::chrono::duration<double, std::micro>(t2 - t1).count();
            double tot_us = std::chrono::duration<double, std::micro>(t2 - t0).count();

            read_times.push_back(read_us);
            detect_times.push_back(det_us);
            total_times.push_back(tot_us);
            i++;
        }
    } else {
        // Mode A: Raw YUV Y-plane path (C++ native)
        ws_camerad::FrameReader reader("/ws_camerad_frames");
        if (!reader.connect()) {
            std::cerr << "Failed to connect to raw shared memory.\n";
            return 1;
        }
        if (min_interval_ms > 0) reader.set_min_interval(min_interval_ms);
        w = reader.width();
        h = reader.height();

        MotionDetector detector(threshold, min_area, scale);
        detector.init(w, h);

        std::cout << "=== C++ Motion Benchmark (YUV Y-plane path) ===\n"
                  << "Resolution: " << w << "x" << h
                  << " → " << w/scale << "x" << h/scale << "\n"
                  << "Frames: " << num_frames << "\n"
                  << "Threshold: " << threshold << ", MinArea: " << min_area << "\n"
                  << "Scale: " << scale << "x\n\n";

        using clock = std::chrono::high_resolution_clock;

        for (int i = 0; i < num_frames && g_running; ) {
            auto t0 = clock::now();
            auto frame = reader.read_frame(100);
            if (!frame) continue;
            auto t1 = clock::now();

            cv::Mat y_plane(h, w, CV_8UC1, const_cast<uint8_t*>(frame->data), frame->stride);
            bool motion = detector.detect(y_plane);
            auto t2 = clock::now();

            if (motion) motion_count++;

            double read_us = std::chrono::duration<double, std::micro>(t1 - t0).count();
            double det_us = std::chrono::duration<double, std::micro>(t2 - t1).count();
            double tot_us = std::chrono::duration<double, std::micro>(t2 - t0).count();

            read_times.push_back(read_us);
            detect_times.push_back(det_us);
            total_times.push_back(tot_us);
            i++;
        }
    }

    // Print results
    auto read_s = compute_stats(read_times, 0);
    auto det_s = compute_stats(detect_times, motion_count);
    auto tot_s = compute_stats(total_times, motion_count);

    auto print_row = [](const char* label, const Stats& s) {
        printf("  %-14s %8.0f %8.0f %8.0f %8.0f %8.0f %8.0f\n",
               label, s.mean_us, s.median_us, s.stddev_us, s.p95_us, s.min_us, s.max_us);
    };

    printf("\nResults (%d frames, %d motion):\n", det_s.total_frames, motion_count);
    printf("  %-14s %8s %8s %8s %8s %8s %8s\n",
           "Phase", "Mean", "Median", "StdDev", "P95", "Min", "Max");
    printf("  %-14s %8s %8s %8s %8s %8s %8s\n",
           "", "(µs)", "(µs)", "(µs)", "(µs)", "(µs)", "(µs)");
    printf("  %s\n", std::string(70, '-').c_str());
    print_row("Frame read", read_s);
    print_row("Detection", det_s);
    print_row("Total", tot_s);

    printf("\n  Throughput: %.1f fps (detection only)\n", 1e6 / det_s.mean_us);
    printf("  Throughput: %.1f fps (total incl. read)\n", 1e6 / tot_s.mean_us);

    return 0;
}
