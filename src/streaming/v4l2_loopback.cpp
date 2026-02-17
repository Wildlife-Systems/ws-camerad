#include "camera_daemon/v4l2_loopback.hpp"
#include "camera_daemon/logger.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <linux/videodev2.h>
#include <cstring>
#include <dirent.h>
#include <fstream>

namespace camera_daemon {

V4L2LoopbackOutput::V4L2LoopbackOutput() = default;

V4L2LoopbackOutput::~V4L2LoopbackOutput() {
    close();
}

bool V4L2LoopbackOutput::initialize(const Config& config) {
    config_ = config;

    LOG_INFO("Initializing v4l2loopback output: ", config.device, 
             " (", config.width, "x", config.height, ")");

    if (!open_device()) {
        return false;
    }

    if (!set_format()) {
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    LOG_INFO("v4l2loopback output ready: ", config.device);
    return true;
}

void V4L2LoopbackOutput::setup_downsample(uint32_t source_width, uint32_t source_height) {
    source_width_ = source_width;
    source_height_ = source_height;
    needs_downsample_ = (source_width != config_.width || source_height != config_.height);
    if (needs_downsample_) {
        size_t dst_size = config_.width * config_.height * 3 / 2;
        downsample_buf_.resize(dst_size);
        LOG_INFO("v4l2loopback ", config_.device, ": downsampling ",
                 source_width, "x", source_height, " -> ",
                 config_.width, "x", config_.height);
    }
}

bool V4L2LoopbackOutput::open_device() {
    // Use O_NONBLOCK to prevent write() from blocking when no readers connected
    // This avoids stalling the frame callback if vcam consumers are slow/absent
    fd_ = open(config_.device.c_str(), O_RDWR | O_NONBLOCK);
    if (fd_ < 0) {
        LOG_ERROR("Failed to open v4l2loopback device: ", config_.device, " - ", strerror(errno));
        return false;
    }

    // Query capabilities to verify it's an output device
    struct v4l2_capability cap;
    if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        LOG_ERROR("Failed to query device capabilities: ", strerror(errno));
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    // v4l2loopback devices support both capture (for readers) and output (for writers)
    if (!(cap.capabilities & V4L2_CAP_VIDEO_OUTPUT)) {
        LOG_ERROR("Device ", config_.device, " does not support video output");
        ::close(fd_);
        fd_ = -1;
        return false;
    }

    LOG_DEBUG("v4l2loopback device: ", reinterpret_cast<char*>(cap.card),
              " driver: ", reinterpret_cast<char*>(cap.driver));

    return true;
}

bool V4L2LoopbackOutput::set_format() {
    struct v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    fmt.fmt.pix.width = config_.width;
    fmt.fmt.pix.height = config_.height;
    fmt.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;  // Native camera format - no conversion needed
    fmt.fmt.pix.field = V4L2_FIELD_NONE;
    fmt.fmt.pix.bytesperline = config_.width;  // Y stride
    fmt.fmt.pix.sizeimage = config_.width * config_.height * 3 / 2;  // YUV420 size
    fmt.fmt.pix.colorspace = V4L2_COLORSPACE_SRGB;

    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        LOG_ERROR("Failed to set output format: ", strerror(errno));
        return false;
    }

    // Verify the format was accepted
    if (fmt.fmt.pix.width != config_.width || fmt.fmt.pix.height != config_.height) {
        LOG_WARN("v4l2loopback adjusted resolution to ", 
                 fmt.fmt.pix.width, "x", fmt.fmt.pix.height);
        config_.width = fmt.fmt.pix.width;
        config_.height = fmt.fmt.pix.height;
    }

    // Set frame rate hint
    struct v4l2_streamparm parm{};
    parm.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    parm.parm.output.timeperframe.numerator = 1;
    parm.parm.output.timeperframe.denominator = config_.framerate;
    if (ioctl(fd_, VIDIOC_S_PARM, &parm) < 0) {
        LOG_DEBUG("Could not set frame rate hint (non-fatal)");
    }

    LOG_DEBUG("v4l2loopback format set: ", config_.width, "x", config_.height, " YUV420");
    return true;
}

bool V4L2LoopbackOutput::write_frame(const uint8_t* yuv_data, size_t size, 
                                      const FrameMetadata& metadata) {
    if (fd_ < 0) {
        return false;
    }

    // Lazy init: detect source dimensions from first frame
    if (source_width_ == 0) {
        setup_downsample(metadata.width, metadata.height);
    }

    const uint8_t* out_data = yuv_data;
    size_t out_size = size;

    if (needs_downsample_) {
        downsample_yuv420(yuv_data, metadata.width, metadata.height,
                          metadata.stride, downsample_buf_.data(),
                          config_.width, config_.height);
        out_data = downsample_buf_.data();
        out_size = downsample_buf_.size();
    }

    // Write YUV420 frame
    ssize_t written = write(fd_, out_data, out_size);
    if (written < 0) {
        if (errno == EAGAIN || errno == EWOULDBLOCK) {
            // No readers connected - this is normal, just skip
            frames_dropped_++;
            return true;
        }
        LOG_ERROR("Failed to write to v4l2loopback: ", strerror(errno));
        frames_dropped_++;
        return false;
    }

    if (static_cast<size_t>(written) != out_size) {
        LOG_WARN("Partial write to v4l2loopback: ", written, "/", out_size);
        frames_dropped_++;
        return false;
    }

    frames_written_++;
    bytes_written_ += written;
    return true;
}

void V4L2LoopbackOutput::close() {
    if (fd_ >= 0) {
        ::close(fd_);
        fd_ = -1;
        LOG_DEBUG("Closed v4l2loopback device: ", config_.device);
    }
}

V4L2LoopbackOutput::Stats V4L2LoopbackOutput::get_stats() const {
    return {
        frames_written_.load(),
        frames_dropped_.load(),
        bytes_written_.load()
    };
}

bool V4L2LoopbackOutput::is_available() {
    // Check if v4l2loopback module is loaded
    std::ifstream modules("/proc/modules");
    std::string line;
    while (std::getline(modules, line)) {
        if (line.find("v4l2loopback") != std::string::npos) {
            return true;
        }
    }
    return false;
}

void V4L2LoopbackOutput::downsample_yuv420(const uint8_t* src, uint32_t src_w,
                                            uint32_t src_h, uint32_t src_stride,
                                            uint8_t* dst, uint32_t dst_w,
                                            uint32_t dst_h) {
    // Nearest-neighbour downsample of YUV420 planar
    // Y plane
    const uint8_t* src_y = src;
    uint8_t* dst_y = dst;
    for (uint32_t dy = 0; dy < dst_h; dy++) {
        uint32_t sy = dy * src_h / dst_h;
        const uint8_t* src_row = src_y + sy * src_stride;
        for (uint32_t dx = 0; dx < dst_w; dx++) {
            uint32_t sx = dx * src_w / dst_w;
            dst_y[dy * dst_w + dx] = src_row[sx];
        }
    }

    // U plane
    uint32_t src_uv_stride = src_stride / 2;
    uint32_t dst_uv_w = dst_w / 2;
    uint32_t dst_uv_h = dst_h / 2;
    const uint8_t* src_u = src_y + src_stride * src_h;
    uint8_t* dst_u = dst_y + dst_w * dst_h;
    for (uint32_t dy = 0; dy < dst_uv_h; dy++) {
        uint32_t sy = dy * (src_h / 2) / dst_uv_h;
        const uint8_t* src_row = src_u + sy * src_uv_stride;
        for (uint32_t dx = 0; dx < dst_uv_w; dx++) {
            uint32_t sx = dx * (src_w / 2) / dst_uv_w;
            dst_u[dy * dst_uv_w + dx] = src_row[sx];
        }
    }

    // V plane
    const uint8_t* src_v = src_u + src_uv_stride * (src_h / 2);
    uint8_t* dst_v = dst_u + dst_uv_w * dst_uv_h;
    for (uint32_t dy = 0; dy < dst_uv_h; dy++) {
        uint32_t sy = dy * (src_h / 2) / dst_uv_h;
        const uint8_t* src_row = src_v + sy * src_uv_stride;
        for (uint32_t dx = 0; dx < dst_uv_w; dx++) {
            uint32_t sx = dx * (src_w / 2) / dst_uv_w;
            dst_v[dy * dst_uv_w + dx] = src_row[sx];
        }
    }
}

std::vector<std::string> V4L2LoopbackOutput::find_loopback_devices() {
    std::vector<std::string> devices;
    
    DIR* dir = opendir("/dev");
    if (!dir) {
        return devices;
    }

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strncmp(entry->d_name, "video", 5) != 0) {
            continue;
        }

        std::string device_path = std::string("/dev/") + entry->d_name;
        int fd = open(device_path.c_str(), O_RDWR);
        if (fd < 0) {
            continue;
        }

        struct v4l2_capability cap;
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) == 0) {
            // v4l2loopback devices have "v4l2 loopback" as driver
            if (strcmp(reinterpret_cast<char*>(cap.driver), "v4l2 loopback") == 0) {
                devices.push_back(device_path);
            }
        }
        ::close(fd);
    }

    closedir(dir);
    return devices;
}

} // namespace camera_daemon
