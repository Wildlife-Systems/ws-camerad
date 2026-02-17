/**
 * Demo C++ Frame Consumer
 * 
 * This example demonstrates how to consume raw video frames
 * from the camera daemon via shared memory.
 */

// Try installed header first, fall back to local
#if __has_include(<ws_camerad/client.hpp>)
#include <ws_camerad/client.hpp>
#else
#include "../include/ws_camerad/client.hpp"
#endif

#include <iostream>
#include <iomanip>
#include <chrono>
#include <thread>
#include <csignal>
#include <atomic>

volatile bool running = true;

void signal_handler(int) {
    running = false;
}

int main(int argc, char* argv[]) {
    const char* shm_name = (argc > 1) ? argv[1] : ws_camerad::DEFAULT_SHM_NAME;
    
    if (argc > 1 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        std::cout << "Usage: " << argv[0] << " [shm_name]" << std::endl;
        std::cout << "  shm_name: Shared memory name (default: " << ws_camerad::DEFAULT_SHM_NAME << ")" << std::endl;
        std::cout << "            Use " << ws_camerad::DEFAULT_BGR_SHM_NAME << " for BGR frames" << std::endl;
        return 0;
    }
    
    std::cout << "Frame Consumer Demo" << std::endl;
    std::cout << "Opening shared memory: " << shm_name << std::endl;

    // Set up signal handler
    std::signal(SIGINT, signal_handler);
    std::signal(SIGTERM, signal_handler);

    ws_camerad::FrameReader reader(shm_name);
    
    if (!reader.connect()) {
        std::cerr << "Failed to connect to shared memory." << std::endl;
        std::cerr << "Is the camera daemon running with raw_sharing enabled?" << std::endl;
        return 1;
    }

    std::cout << "Connected to shared memory" << std::endl;
    std::cout << "Resolution: " << reader.width() << "x" << reader.height() << std::endl;
    std::cout << "Frame size: " << reader.frame_size() << " bytes" << std::endl;
    std::cout << std::endl;
    std::cout << "Press Ctrl+C to stop" << std::endl;
    std::cout << std::endl;

    uint64_t frame_count = 0;
    auto start_time = std::chrono::steady_clock::now();
    auto last_print = start_time;

    while (running) {
        // Try to read a frame with 100ms timeout
        auto frame = reader.read_frame(100);
        
        if (frame) {
            frame_count++;

            // Here you would process the frame data:
            // - frame->data points to the raw pixel data
            // - frame->width, frame->height for dimensions
            // - frame->stride for bytes per row
            // - frame->timestamp_us for capture time
            
            // For this demo, we just count frames
        }

        // Print statistics every second
        auto now = std::chrono::steady_clock::now();
        auto elapsed = std::chrono::duration_cast<std::chrono::seconds>(now - last_print);
        
        if (elapsed.count() >= 1) {
            auto total_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time);
            double fps = frame_count * 1000.0 / total_elapsed.count();
            
            std::cout << "\rFrames: " << frame_count 
                      << " | FPS: " << std::fixed << std::setprecision(1) << fps
                      << "   " << std::flush;
            
            last_print = now;
        }
    }

    std::cout << std::endl << "Shutting down..." << std::endl;
    std::cout << "Total frames consumed: " << frame_count << std::endl;
    
    return 0;
}
