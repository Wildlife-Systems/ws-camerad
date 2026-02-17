#include "camera_daemon/v4l2_jpeg.hpp"
#include "camera_daemon/logger.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <cstring>
#include <poll.h>

namespace camera_daemon {

namespace {
    constexpr const char* JPEG_ENCODER_DEVICE = "/dev/video31";
}

V4L2JpegEncoder::V4L2JpegEncoder() = default;

V4L2JpegEncoder::~V4L2JpegEncoder() {
    stream_off();
    
    if (input_buffer_.data) {
        munmap(input_buffer_.data, input_buffer_.capacity);
        input_buffer_.data = nullptr;
    }
    if (output_buffer_.data) {
        munmap(output_buffer_.data, output_buffer_.capacity);
        output_buffer_.data = nullptr;
    }
    
    if (fd_ >= 0) {
        // Sync any pending operations before close
        fsync(fd_);
        close(fd_);
        fd_ = -1;
    }
}

bool V4L2JpegEncoder::is_available() {
    int fd = open(JPEG_ENCODER_DEVICE, O_RDWR);
    if (fd < 0) {
        return false;
    }
    close(fd);
    return true;
}

bool V4L2JpegEncoder::initialize(const Config& config) {
    config_ = config;
    
    LOG_INFO("Initializing hardware JPEG encoder: ", config.width, "x", config.height,
             ", quality=", config.quality);

    if (!open_device()) return false;
    if (!setup_formats()) return false;
    if (!allocate_buffers()) return false;

    LOG_INFO("Hardware JPEG encoder initialized");
    return true;
}

bool V4L2JpegEncoder::open_device() {
    // Retry opening device - previous user may still be releasing resources
    for (int attempt = 0; attempt < 5; ++attempt) {
        fd_ = open(JPEG_ENCODER_DEVICE, O_RDWR);
        if (fd_ >= 0) break;
        if (errno == EBUSY && attempt < 4) {
            usleep(10000);  // 10ms
            continue;
        }
        LOG_ERROR("Failed to open JPEG encoder device: ", strerror(errno));
        return false;
    }

    struct v4l2_capability cap;
    if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        LOG_ERROR("Failed to query JPEG encoder capabilities");
        return false;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)) {
        LOG_ERROR("JPEG encoder does not support M2M");
        return false;
    }

    LOG_DEBUG("JPEG encoder: ", reinterpret_cast<char*>(cap.card));
    return true;
}

bool V4L2JpegEncoder::setup_formats() {
    // Set input format (YUV420 planar)
    struct v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.width = config_.width;
    fmt.fmt.pix_mp.height = config_.height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.colorspace = V4L2_COLORSPACE_JPEG;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = config_.width * config_.height * 3 / 2;
    fmt.fmt.pix_mp.plane_fmt[0].bytesperline = config_.width;

    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        LOG_ERROR("Failed to set JPEG input format: ", strerror(errno));
        return false;
    }

    // Set output format (JPEG)
    fmt = {};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = config_.width;
    fmt.fmt.pix_mp.height = config_.height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_JPEG;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.colorspace = V4L2_COLORSPACE_JPEG;
    fmt.fmt.pix_mp.num_planes = 1;
    // JPEG output can be larger than raw, allocate generously
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = config_.width * config_.height;

    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        LOG_ERROR("Failed to set JPEG output format: ", strerror(errno));
        return false;
    }

    // Set quality via control
    struct v4l2_control ctrl{};
    ctrl.id = V4L2_CID_JPEG_COMPRESSION_QUALITY;
    ctrl.value = config_.quality;
    if (ioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
        LOG_WARN("Failed to set JPEG quality: ", strerror(errno));
    }

    return true;
}

bool V4L2JpegEncoder::allocate_buffers() {
    // Request 1 input buffer
    struct v4l2_requestbuffers reqbufs{};
    reqbufs.count = 1;
    reqbufs.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    reqbufs.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd_, VIDIOC_REQBUFS, &reqbufs) < 0) {
        LOG_ERROR("Failed to request JPEG input buffers: ", strerror(errno));
        return false;
    }

    struct v4l2_buffer buf{};
    struct v4l2_plane planes[1]{};
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    buf.length = 1;
    buf.m.planes = planes;

    if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
        LOG_ERROR("Failed to query JPEG input buffer: ", strerror(errno));
        return false;
    }

    input_buffer_.capacity = buf.m.planes[0].length;
    input_buffer_.data = mmap(nullptr, buf.m.planes[0].length,
                               PROT_READ | PROT_WRITE, MAP_SHARED,
                               fd_, buf.m.planes[0].m.mem_offset);
    if (input_buffer_.data == MAP_FAILED) {
        LOG_ERROR("Failed to mmap JPEG input buffer: ", strerror(errno));
        return false;
    }

    // Request 1 output buffer
    reqbufs.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd_, VIDIOC_REQBUFS, &reqbufs) < 0) {
        LOG_ERROR("Failed to request JPEG output buffers: ", strerror(errno));
        return false;
    }

    buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    buf.length = 1;
    buf.m.planes = planes;

    if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
        LOG_ERROR("Failed to query JPEG output buffer: ", strerror(errno));
        return false;
    }

    output_buffer_.capacity = buf.m.planes[0].length;
    output_buffer_.data = mmap(nullptr, buf.m.planes[0].length,
                                PROT_READ | PROT_WRITE, MAP_SHARED,
                                fd_, buf.m.planes[0].m.mem_offset);
    if (output_buffer_.data == MAP_FAILED) {
        LOG_ERROR("Failed to mmap JPEG output buffer: ", strerror(errno));
        return false;
    }

    LOG_DEBUG("JPEG encoder buffers allocated: input=", input_buffer_.capacity,
              " output=", output_buffer_.capacity);
    return true;
}

bool V4L2JpegEncoder::stream_on() {
    if (streaming_) return true;

    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        LOG_ERROR("Failed to start JPEG input stream: ", strerror(errno));
        return false;
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        LOG_ERROR("Failed to start JPEG output stream: ", strerror(errno));
        return false;
    }

    streaming_ = true;
    return true;
}

void V4L2JpegEncoder::stream_off() {
    if (!streaming_) return;

    int type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ioctl(fd_, VIDIOC_STREAMOFF, &type);

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(fd_, VIDIOC_STREAMOFF, &type);

    // Release buffers back to the driver (prevents driver state issues)
    struct v4l2_requestbuffers reqbufs{};
    reqbufs.count = 0;
    reqbufs.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    reqbufs.memory = V4L2_MEMORY_MMAP;
    ioctl(fd_, VIDIOC_REQBUFS, &reqbufs);

    reqbufs.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    reqbufs.memory = V4L2_MEMORY_MMAP;
    ioctl(fd_, VIDIOC_REQBUFS, &reqbufs);

    streaming_ = false;
}

bool V4L2JpegEncoder::encode(const uint8_t* yuv_data, size_t yuv_size, std::vector<uint8_t>& jpeg_out) {
    if (fd_ < 0) {
        LOG_ERROR("JPEG encoder not initialized");
        return false;
    }

    if (yuv_size > input_buffer_.capacity) {
        LOG_ERROR("YUV data too large: ", yuv_size, " > ", input_buffer_.capacity);
        return false;
    }

    // Start streaming if not already
    if (!streaming_ && !stream_on()) {
        return false;
    }

    // Copy input data
    memcpy(input_buffer_.data, yuv_data, yuv_size);

    // Queue input buffer
    struct v4l2_buffer buf{};
    struct v4l2_plane planes[1]{};
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    buf.length = 1;
    buf.m.planes = planes;
    planes[0].bytesused = yuv_size;
    planes[0].length = input_buffer_.capacity;

    if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        LOG_ERROR("Failed to queue JPEG input buffer: ", strerror(errno));
        return false;
    }

    // Queue output buffer
    buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    buf.length = 1;
    buf.m.planes = planes;
    planes[0].bytesused = 0;
    planes[0].length = output_buffer_.capacity;

    if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        LOG_ERROR("Failed to queue JPEG output buffer: ", strerror(errno));
        return false;
    }

    // Wait for completion
    struct pollfd pfd{};
    pfd.fd = fd_;
    pfd.events = POLLIN;
    
    int ret = poll(&pfd, 1, 1000);  // 1 second timeout
    if (ret <= 0) {
        LOG_ERROR("JPEG encode timeout");
        return false;
    }

    // Dequeue output buffer to get JPEG data
    buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.length = 1;
    buf.m.planes = planes;

    if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        LOG_ERROR("Failed to dequeue JPEG output: ", strerror(errno));
        return false;
    }

    size_t jpeg_size = planes[0].bytesused;
    jpeg_out.resize(jpeg_size);
    memcpy(jpeg_out.data(), output_buffer_.data, jpeg_size);

    // Dequeue input buffer
    buf = {};
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.length = 1;
    buf.m.planes = planes;

    if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        LOG_ERROR("Failed to dequeue JPEG input: ", strerror(errno));
        // Non-fatal, continue
    }

    LOG_DEBUG("Hardware JPEG encoded: ", yuv_size, " -> ", jpeg_size, " bytes");
    return true;
}

} // namespace camera_daemon
