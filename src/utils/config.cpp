#include "camera_daemon/common.hpp"
#include "camera_daemon/daemon.hpp"
#include "camera_daemon/logger.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <getopt.h>

namespace camera_daemon {

namespace {

std::string trim(const std::string& str) {
    size_t start = str.find_first_not_of(" \t\r\n");
    if (start == std::string::npos) return "";
    size_t end = str.find_last_not_of(" \t\r\n");
    return str.substr(start, end - start + 1);
}

std::pair<std::string, std::string> parse_line(const std::string& line) {
    size_t eq_pos = line.find('=');
    if (eq_pos == std::string::npos) {
        return {"", ""};
    }
    return {
        trim(line.substr(0, eq_pos)),
        trim(line.substr(eq_pos + 1))
    };
}

} // anonymous namespace

DaemonConfig load_config(const std::string& path) {
    DaemonConfig config;
    std::ifstream file(path);
    
    if (!file.is_open()) {
        LOG_WARN("Could not open config file: ", path, ", using defaults");
        return config;
    }

    std::string line;
    std::string section;
    
    // For virtual_camera sections, we track the current one being parsed
    VirtualCameraConfig* current_vcam = nullptr;
    
    while (std::getline(file, line)) {
        line = trim(line);
        
        // Skip empty lines and comments
        if (line.empty() || line[0] == '#' || line[0] == ';') {
            continue;
        }
        
        // Section header
        if (line[0] == '[' && line.back() == ']') {
            section = line.substr(1, line.size() - 2);
            current_vcam = nullptr;  // Reset on new section
            
            // Check for virtual_camera section (e.g., [virtual_camera.0] or [virtual_camera])
            if (section.rfind("virtual_camera", 0) == 0) {
                config.virtual_cameras.emplace_back();
                current_vcam = &config.virtual_cameras.back();
            }
            continue;
        }
        
        auto [key, value] = parse_line(line);
        if (key.empty()) continue;

        // Parse based on section
        if (section == "daemon" || section.empty()) {
            if (key == "socket_path") config.socket_path = value;
            else if (key == "stills_dir") config.stills_dir = value;
            else if (key == "clips_dir") config.clips_dir = value;
            else if (key == "shm_name") config.shm_name = value;
            else if (key == "bgr_shm_name") config.bgr_shm_name = value;
            else if (key == "ring_buffer_seconds") config.ring_buffer_seconds = std::stoul(value);
            else if (key == "post_event_seconds") config.post_event_seconds = std::stoul(value);
            else if (key == "enable_rtsp") config.enable_rtsp = (value == "true" || value == "1");
            else if (key == "rtsp_port") config.rtsp_port = std::stoul(value);
            else if (key == "enable_raw_sharing") config.enable_raw_sharing = (value == "true" || value == "1");
            else if (key == "enable_bgr_sharing") config.enable_bgr_sharing = (value == "true" || value == "1");
        }
        else if (section == "camera") {
            if (key == "width") config.camera.width = std::stoul(value);
            else if (key == "height") config.camera.height = std::stoul(value);
            else if (key == "framerate") config.camera.framerate = std::stoul(value);
            else if (key == "bitrate") config.camera.bitrate = std::stoul(value);
            else if (key == "keyframe_interval") config.camera.keyframe_interval = std::stoul(value);
            else if (key == "jpeg_quality") config.camera.jpeg_quality = std::stoul(value);
            else if (key == "tuning_file") config.camera.tuning_file = value;
            else if (key == "rotation") {
                uint32_t r = std::stoul(value);
                if (r == 0 || r == 90 || r == 180 || r == 270) {
                    config.camera.rotation = r;
                } else {
                    LOG_WARN("Invalid rotation value: ", value, " (must be 0, 90, 180, or 270)");
                }
            }
            else if (key == "hflip") config.camera.hflip = (value == "true" || value == "1");
            else if (key == "vflip") config.camera.vflip = (value == "true" || value == "1");
        }
        else if (current_vcam != nullptr) {
            // Parsing a virtual_camera section
            if (key == "device") current_vcam->device = value;
            else if (key == "label") current_vcam->label = value;
            else if (key == "enabled") current_vcam->enabled = (value == "true" || value == "1");
            else if (key == "width") current_vcam->width = std::stoul(value);
            else if (key == "height") current_vcam->height = std::stoul(value);
        }
    }
    
    // Log virtual camera configuration
    if (!config.virtual_cameras.empty()) {
        LOG_INFO("Configured ", config.virtual_cameras.size(), " virtual camera(s)");
        for (const auto& vcam : config.virtual_cameras) {
            LOG_INFO("  - ", vcam.device, vcam.label.empty() ? "" : " (" + vcam.label + ")");
        }
    }

    LOG_INFO("Loaded config from: ", path);
    return config;
}

DaemonConfig parse_args(int argc, char* argv[]) {
    DaemonConfig config;
    
    static struct option long_options[] = {
        {"config",      required_argument, 0, 'c'},
        {"socket",      required_argument, 0, 's'},
        {"width",       required_argument, 0, 'W'},
        {"height",      required_argument, 0, 'H'},
        {"framerate",   required_argument, 0, 'f'},
        {"bitrate",     required_argument, 0, 'b'},
        {"rtsp-port",   required_argument, 0, 'r'},
        {"tuning-file", required_argument, 0, 't'},
        {"rotation",    required_argument, 0, 'o'},
        {"hflip",       no_argument,       0, 256},
        {"vflip",       no_argument,       0, 257},
        {"no-rtsp",     no_argument,       0, 'R'},
        {"debug",       no_argument,       0, 'd'},
        {"help",        no_argument,       0, 'h'},
        {"version",     no_argument,       0, 'v'},
        {0, 0, 0, 0}
    };

    int opt;
    int option_index = 0;
    std::string config_path;

    while ((opt = getopt_long(argc, argv, "c:s:W:H:f:b:r:t:o:Rdhv", long_options, &option_index)) != -1) {
        switch (opt) {
            case 'c':
                config_path = optarg;
                break;
            case 's':
                config.socket_path = optarg;
                break;
            case 'W':
                config.camera.width = std::stoul(optarg);
                break;
            case 'H':
                config.camera.height = std::stoul(optarg);
                break;
            case 'f':
                config.camera.framerate = std::stoul(optarg);
                break;
            case 'b':
                config.camera.bitrate = std::stoul(optarg);
                break;
            case 'r':
                config.rtsp_port = std::stoul(optarg);
                break;
            case 't':
                config.camera.tuning_file = optarg;
                break;
            case 'o':
                config.camera.rotation = std::stoul(optarg);
                break;
            case 256:
                config.camera.hflip = true;
                break;
            case 257:
                config.camera.vflip = true;
                break;
            case 'R':
                config.enable_rtsp = false;
                break;
            case 'd':
                Logger::instance().set_level(LogLevel::DEBUG);
                break;
            case 'v':
                std::cout << "camera_daemon version " << VERSION << std::endl;
                exit(0);
            case 'h':
            default:
                std::cout << "Usage: " << argv[0] << " [OPTIONS]\n"
                          << "Options:\n"
                          << "  -c, --config FILE      Configuration file path\n"
                          << "  -s, --socket PATH      Control socket path\n"
                          << "  -W, --width WIDTH      Video width (default: 1280)\n"
                          << "  -H, --height HEIGHT    Video height (default: 720)\n"
                          << "  -f, --framerate FPS    Frame rate (default: 30)\n"
                          << "  -b, --bitrate BPS      Bitrate (default: 4000000)\n"
                          << "  -t, --tuning-file FILE Tuning file for NoIR modules\n"
                          << "  -o, --rotation DEGREES Frame rotation (0, 90, 180, 270)\n"
                          << "      --hflip            Horizontal flip (mirror)\n"
                          << "      --vflip            Vertical flip\n"
                          << "  -r, --rtsp-port PORT   RTSP server port (default: 8554)\n"
                          << "  -R, --no-rtsp          Disable RTSP streaming\n"
                          << "  -d, --debug            Enable debug logging\n"
                          << "  -h, --help             Show this help\n"
                          << "  -v, --version          Show version\n";
                exit(opt == 'h' ? 0 : 1);
        }
    }

    // Load config file if specified, then override with command line args
    if (!config_path.empty()) {
        config = load_config(config_path);
        
        // Re-parse to override with command line
        optind = 1;  // Reset getopt
        while ((opt = getopt_long(argc, argv, "c:s:W:H:f:b:r:t:o:Rdhv", long_options, &option_index)) != -1) {
            switch (opt) {
                case 's': config.socket_path = optarg; break;
                case 'W': config.camera.width = std::stoul(optarg); break;
                case 'H': config.camera.height = std::stoul(optarg); break;
                case 'f': config.camera.framerate = std::stoul(optarg); break;
                case 'b': config.camera.bitrate = std::stoul(optarg); break;
                case 'r': config.rtsp_port = std::stoul(optarg); break;
                case 't': config.camera.tuning_file = optarg; break;
                case 'o': config.camera.rotation = std::stoul(optarg); break;
                case 256: config.camera.hflip = true; break;
                case 257: config.camera.vflip = true; break;
                case 'R': config.enable_rtsp = false; break;
            }
        }
    }

    return config;
}

} // namespace camera_daemon
