/**
 * Demo C++ Clip Client
 * 
 * This example demonstrates how to connect to the camera daemon
 * and trigger a video clip extraction via the control socket.
 * 
 * Usage:
 *   ./clip_client [start_offset end_offset]
 * 
 * CLIP command syntax: CLIP <start_offset> <end_offset>
 *   - start_offset: Start time relative to now (negative=past, positive=future)
 *   - end_offset: End time relative to now (negative=past, positive=future)
 * 
 * Examples:
 *   ./clip_client -5 5     # 10 seconds: from 5s ago to 5s from now
 *   ./clip_client -10 0    # 10 seconds: from 10s ago to now (all from buffer)
 *   ./clip_client -3 2     # 5 seconds: from 3s ago to 2s from now
 */

// Try installed header first, fall back to local
#if __has_include(<ws_camerad/client.hpp>)
#include <ws_camerad/client.hpp>
#else
#include "../include/ws_camerad/client.hpp"
#endif

#include <iostream>
#include <cstdlib>

constexpr int DEFAULT_START_OFFSET = -5;  // 5 seconds ago
constexpr int DEFAULT_END_OFFSET = 5;     // 5 seconds from now

int main(int argc, char* argv[]) {
    int start_offset = DEFAULT_START_OFFSET;
    int end_offset = DEFAULT_END_OFFSET;
    
    // Parse arguments
    for (int i = 1; i < argc; i++) {
        if (std::string(argv[i]) == "-h" || std::string(argv[i]) == "--help") {
            std::cout << "Usage: " << argv[0] << " [start_offset end_offset]" << std::endl;
            std::cout << "  start_offset  Start time relative to now (default: " << DEFAULT_START_OFFSET << ")" << std::endl;
            std::cout << "  end_offset    End time relative to now (default: " << DEFAULT_END_OFFSET << ")" << std::endl;
            std::cout << "\nExamples:" << std::endl;
            std::cout << "  " << argv[0] << " -5 5     # 10s: from 5s ago to 5s from now" << std::endl;
            std::cout << "  " << argv[0] << " -10 0    # 10s: from 10s ago to now (all buffer)" << std::endl;
            std::cout << "  " << argv[0] << " -3 2     # 5s: from 3s ago to 2s from now" << std::endl;
            return 0;
        } else if (i + 1 < argc) {
            // Check if this looks like a number
            bool looks_like_number = (argv[i][0] >= '0' && argv[i][0] <= '9') ||
                                     (argv[i][0] == '-' && argv[i][1] >= '0' && argv[i][1] <= '9');
            if (looks_like_number) {
                start_offset = std::atoi(argv[i]);
                end_offset = std::atoi(argv[++i]);
            }
        }
    }
    
    int duration = end_offset - start_offset;
    
    ws_camerad::Client client;
    
    std::cout << "Connecting to camera daemon..." << std::endl;
    
    if (!client.connect()) {
        std::cerr << "Failed to connect. Is the camera daemon running?" << std::endl;
        return 1;
    }
    
    std::cout << "Requesting clip (from " << start_offset << "s to " << end_offset 
              << "s, " << duration << "s total)..." << std::endl;
    
    if (end_offset > 0) {
        std::cout << "Recording post-event footage, this may take a moment..." << std::endl;
    }
    
    auto response = client.capture_clip(start_offset, end_offset);
    
    std::cout << "Daemon response: " << response.raw() << std::endl;
    
    if (response.ok()) {
        std::cout << "Clip extraction successful!" << std::endl;
        if (!response.path().empty()) {
            std::cout << "Saved to: " << response.path() << std::endl;
        }
        return 0;
    } else {
        std::cerr << "Clip extraction failed: " << response.error() << std::endl;
        return 1;
    }
}
