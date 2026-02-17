/**
 * Demo C++ Still Client
 * 
 * This example demonstrates how to connect to the camera daemon
 * and trigger a still capture via the control socket.
 * 
 * Usage:
 *   ./still_client [time_offset]
 * 
 * Examples:
 *   ./still_client              # Capture now
 *   ./still_client -2           # Capture from 2 seconds ago
 */

// Try installed header first, fall back to local
#if __has_include(<ws_camerad/client.hpp>)
#include <ws_camerad/client.hpp>
#else
#include "../include/ws_camerad/client.hpp"
#endif

#include <iostream>
#include <cstdlib>

int main(int argc, char* argv[]) {
    int time_offset = (argc > 1) ? std::atoi(argv[1]) : 0;
    
    if (argc > 1 && (std::string(argv[1]) == "-h" || std::string(argv[1]) == "--help")) {
        std::cout << "Usage: " << argv[0] << " [time_offset]" << std::endl;
        std::cout << "  time_offset: Seconds offset (negative=past, 0=now, positive=future)" << std::endl;
        std::cout << "\nExamples:" << std::endl;
        std::cout << "  " << argv[0] << "           # Capture now" << std::endl;
        std::cout << "  " << argv[0] << " -2        # Capture from 2 seconds ago" << std::endl;
        return 0;
    }
    
    ws_camerad::Client client;
    
    std::cout << "Connecting to camera daemon..." << std::endl;
    
    if (!client.connect()) {
        std::cerr << "Failed to connect. Is the camera daemon running?" << std::endl;
        return 1;
    }
    
    if (time_offset == 0) {
        std::cout << "Requesting still capture..." << std::endl;
    } else if (time_offset < 0) {
        std::cout << "Requesting still from " << -time_offset << "s ago..." << std::endl;
    } else {
        std::cout << "Requesting still " << time_offset << "s from now..." << std::endl;
    }
    
    auto response = client.capture_still(time_offset);
    
    std::cout << "Daemon response: " << response.raw() << std::endl;
    
    if (response.ok()) {
        std::cout << "Still capture successful!" << std::endl;
        if (!response.path().empty()) {
            std::cout << "Saved to: " << response.path() << std::endl;
        }
        return 0;
    } else {
        std::cerr << "Still capture failed: " << response.error() << std::endl;
        return 1;
    }
}
