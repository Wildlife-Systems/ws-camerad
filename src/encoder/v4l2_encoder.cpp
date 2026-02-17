#include "camera_daemon/v4l2_encoder.hpp"
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
    constexpr const char* ENCODER_DEVICE = "/dev/video11";  // RPi H264 encoder
    constexpr int NUM_INPUT_BUFFERS = 4;
    constexpr int NUM_OUTPUT_BUFFERS = 4;
}

V4L2Encoder::V4L2Encoder() = default;

V4L2Encoder::~V4L2Encoder() {
    stop();
    
    // Unmap output buffers before releasing
    for (auto& buf : output_buffers_) {
        if (buf.data) {
            munmap(buf.data, buf.capacity);
            buf.data = nullptr;
        }
    }
    
    // Unmap input buffers if using MMAP mode (software rotation)
    for (auto& buf : input_buffers_) {
        if (buf.data) {
            munmap(buf.data, buf.capacity);
            buf.data = nullptr;
        }
    }
    
    // Release buffers back to the driver (even if never started)
    // This is critical - buffers allocated by initialize() must be freed
    if (fd_ >= 0) {
        struct v4l2_requestbuffers reqbufs{};
        reqbufs.count = 0;
        reqbufs.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        reqbufs.memory = config_.use_userptr ? V4L2_MEMORY_MMAP : V4L2_MEMORY_DMABUF;
        ioctl(fd_, VIDIOC_REQBUFS, &reqbufs);

        reqbufs.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        reqbufs.memory = V4L2_MEMORY_MMAP;
        ioctl(fd_, VIDIOC_REQBUFS, &reqbufs);
        
        close(fd_);
    }
}

bool V4L2Encoder::initialize(const Config& config) {
    config_ = config;
    
    LOG_INFO("Initializing V4L2 encoder: ", config.width, "x", config.height, 
             "@", config.framerate, "fps, ", config.bitrate/1000, "kbps");

    if (!open_device()) return false;
    if (!setup_output_format()) return false;  // Output (encoded) first
    if (!setup_input_format()) return false;   // Input (raw) second
    if (!setup_controls()) return false;
    if (!allocate_buffers()) return false;

    LOG_INFO("V4L2 encoder initialized successfully");
    return true;
}

bool V4L2Encoder::open_device() {
    // Retry opening device - previous user may still be releasing resources
    for (int attempt = 0; attempt < 5; ++attempt) {
        fd_ = open(ENCODER_DEVICE, O_RDWR | O_NONBLOCK);
        if (fd_ >= 0) break;
        if (errno == EBUSY && attempt < 4) {
            usleep(10000);  // 10ms
            continue;
        }
        LOG_ERROR("Failed to open encoder device: ", ENCODER_DEVICE, " - ", strerror(errno));
        return false;
    }

    struct v4l2_capability cap;
    if (ioctl(fd_, VIDIOC_QUERYCAP, &cap) < 0) {
        LOG_ERROR("Failed to query device capabilities");
        return false;
    }

    if (!(cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)) {
        LOG_ERROR("Device does not support M2M");
        return false;
    }

    LOG_DEBUG("Encoder device: ", reinterpret_cast<char*>(cap.card));
    return true;
}

bool V4L2Encoder::setup_input_format() {
    struct v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    fmt.fmt.pix_mp.width = config_.width;
    fmt.fmt.pix_mp.height = config_.height;
    fmt.fmt.pix_mp.pixelformat = V4L2_PIX_FMT_YUV420;  // YUV420 planar (matches libcamera output)
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = config_.width * config_.height * 3 / 2;
    fmt.fmt.pix_mp.plane_fmt[0].bytesperline = config_.width;

    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        LOG_ERROR("Failed to set input format: ", strerror(errno));
        return false;
    }

    uint32_t w = fmt.fmt.pix_mp.width;
    uint32_t h = fmt.fmt.pix_mp.height;
    LOG_DEBUG("Input format set: ", w, "x", h);
    return true;
}

bool V4L2Encoder::setup_output_format() {
    struct v4l2_format fmt{};
    fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    fmt.fmt.pix_mp.width = config_.width;
    fmt.fmt.pix_mp.height = config_.height;
    fmt.fmt.pix_mp.pixelformat = (config_.codec == Codec::H265) ? 
                                  V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;
    fmt.fmt.pix_mp.field = V4L2_FIELD_NONE;
    fmt.fmt.pix_mp.colorspace = V4L2_COLORSPACE_REC709;
    fmt.fmt.pix_mp.num_planes = 1;
    fmt.fmt.pix_mp.plane_fmt[0].sizeimage = config_.width * config_.height;  // Compressed size estimate

    if (ioctl(fd_, VIDIOC_S_FMT, &fmt) < 0) {
        LOG_ERROR("Failed to set output format: ", strerror(errno));
        return false;
    }

    LOG_DEBUG("Output format set: ", (config_.codec == Codec::H265 ? "H.265" : "H.264"));
    return true;
}

bool V4L2Encoder::setup_controls() {
    // Set bitrate
    struct v4l2_control ctrl{};
    ctrl.id = V4L2_CID_MPEG_VIDEO_BITRATE;
    ctrl.value = config_.bitrate;
    if (ioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
        LOG_WARN("Failed to set bitrate: ", strerror(errno));
    }

    // Set GOP size (keyframe interval)
    ctrl.id = V4L2_CID_MPEG_VIDEO_H264_I_PERIOD;
    ctrl.value = config_.keyframe_interval;
    if (ioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
        LOG_WARN("Failed to set keyframe interval: ", strerror(errno));
    }

    // Set H.264 profile to High for better quality
    ctrl.id = V4L2_CID_MPEG_VIDEO_H264_PROFILE;
    ctrl.value = V4L2_MPEG_VIDEO_H264_PROFILE_HIGH;
    if (ioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
        LOG_WARN("Failed to set H.264 profile: ", strerror(errno));
    }

    // Set level to 4.0 for 720p30
    ctrl.id = V4L2_CID_MPEG_VIDEO_H264_LEVEL;
    ctrl.value = V4L2_MPEG_VIDEO_H264_LEVEL_4_0;
    if (ioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
        LOG_WARN("Failed to set H.264 level: ", strerror(errno));
    }

    // Enable inline headers (SPS/PPS with each IDR)
    ctrl.id = V4L2_CID_MPEG_VIDEO_REPEAT_SEQ_HEADER;
    ctrl.value = 1;
    if (ioctl(fd_, VIDIOC_S_CTRL, &ctrl) < 0) {
        LOG_WARN("Failed to enable repeat headers: ", strerror(errno));
    }

    return true;
}

bool V4L2Encoder::allocate_buffers() {
    // Request input buffers
    // Note: bcm2835-codec doesn't support USERPTR, so we use MMAP for software rotation
    // and copy the rotated frame data into the mapped buffers
    struct v4l2_requestbuffers reqbufs{};
    reqbufs.count = NUM_INPUT_BUFFERS;
    reqbufs.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    reqbufs.memory = config_.use_userptr ? V4L2_MEMORY_MMAP : V4L2_MEMORY_DMABUF;

    if (ioctl(fd_, VIDIOC_REQBUFS, &reqbufs) < 0) {
        LOG_ERROR("Failed to request input buffers: ", strerror(errno));
        return false;
    }

    input_buffers_.resize(reqbufs.count);
    for (unsigned i = 0; i < reqbufs.count; i++) {
        input_buffers_[i].queued = false;
        input_buffers_[i].data = nullptr;
        input_buffers_[i].capacity = 0;
    }

    // For MMAP mode (software rotation), map the input buffers
    if (config_.use_userptr) {
        for (unsigned i = 0; i < reqbufs.count; i++) {
            struct v4l2_buffer buf{};
            struct v4l2_plane planes[1]{};
            buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
            buf.memory = V4L2_MEMORY_MMAP;
            buf.index = i;
            buf.length = 1;
            buf.m.planes = planes;

            if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
                LOG_ERROR("Failed to query input buffer: ", strerror(errno));
                return false;
            }

            input_buffers_[i].capacity = buf.m.planes[0].length;
            input_buffers_[i].data = mmap(nullptr, buf.m.planes[0].length,
                                          PROT_READ | PROT_WRITE, MAP_SHARED,
                                          fd_, buf.m.planes[0].m.mem_offset);
            if (input_buffers_[i].data == MAP_FAILED) {
                LOG_ERROR("Failed to mmap input buffer: ", strerror(errno));
                input_buffers_[i].data = nullptr;
                return false;
            }
        }
    }

    LOG_INFO("Allocated ", input_buffers_.size(), " ",
             config_.use_userptr ? "MMAP" : "DMABUF",
             " input buffers", config_.use_userptr ? " (copy mode)" : " (zero-copy)");

    // Request output buffers (still MMAP - we need to read encoded data)
    reqbufs.count = NUM_OUTPUT_BUFFERS;
    reqbufs.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    reqbufs.memory = V4L2_MEMORY_MMAP;

    if (ioctl(fd_, VIDIOC_REQBUFS, &reqbufs) < 0) {
        LOG_ERROR("Failed to request output buffers: ", strerror(errno));
        return false;
    }

    output_buffers_.resize(reqbufs.count);

    for (unsigned i = 0; i < reqbufs.count; i++) {
        struct v4l2_buffer buf{};
        struct v4l2_plane planes[1]{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = 1;
        buf.m.planes = planes;

        if (ioctl(fd_, VIDIOC_QUERYBUF, &buf) < 0) {
            LOG_ERROR("Failed to query output buffer: ", strerror(errno));
            return false;
        }

        output_buffers_[i].capacity = buf.m.planes[0].length;
        output_buffers_[i].data = mmap(nullptr, buf.m.planes[0].length,
                                        PROT_READ | PROT_WRITE, MAP_SHARED,
                                        fd_, buf.m.planes[0].m.mem_offset);
        if (output_buffers_[i].data == MAP_FAILED) {
            LOG_ERROR("Failed to mmap output buffer: ", strerror(errno));
            return false;
        }
        output_buffers_[i].queued = false;
    }

    LOG_DEBUG("Allocated ", output_buffers_.size(), " output buffers");
    return true;
}

bool V4L2Encoder::start_streaming() {
    // Queue all output buffers
    for (size_t i = 0; i < output_buffers_.size(); i++) {
        struct v4l2_buffer buf{};
        struct v4l2_plane planes[1]{};
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        buf.length = 1;
        buf.m.planes = planes;
        planes[0].length = output_buffers_[i].capacity;

        if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
            LOG_ERROR("Failed to queue output buffer: ", strerror(errno));
            return false;
        }
        output_buffers_[i].queued = true;
    }

    // Start streaming on both queues
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        LOG_ERROR("Failed to start input stream: ", strerror(errno));
        return false;
    }

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    if (ioctl(fd_, VIDIOC_STREAMON, &type) < 0) {
        LOG_ERROR("Failed to start output stream: ", strerror(errno));
        return false;
    }

    return true;
}

bool V4L2Encoder::start() {
    if (running_) {
        return true;
    }

    if (!start_streaming()) {
        return false;
    }

    running_ = true;

    // Start output processing thread
    output_thread_ = std::thread(&V4L2Encoder::output_thread_func, this);

    LOG_INFO("V4L2 encoder started");
    return true;
}

void V4L2Encoder::stop() {
    if (!running_) {
        return;
    }

    LOG_INFO("Stopping V4L2 encoder");
    running_ = false;

    if (output_thread_.joinable()) {
        output_thread_.join();
    }

    // Stop streaming
    enum v4l2_buf_type type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    ioctl(fd_, VIDIOC_STREAMOFF, &type);
    
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    ioctl(fd_, VIDIOC_STREAMOFF, &type);

    // Release buffers back to the driver (prevents driver state issues)
    struct v4l2_requestbuffers reqbufs{};
    reqbufs.count = 0;
    reqbufs.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    reqbufs.memory = config_.use_userptr ? V4L2_MEMORY_MMAP : V4L2_MEMORY_DMABUF;
    ioctl(fd_, VIDIOC_REQBUFS, &reqbufs);

    reqbufs.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    reqbufs.memory = V4L2_MEMORY_MMAP;
    ioctl(fd_, VIDIOC_REQBUFS, &reqbufs);

    LOG_INFO("V4L2 encoder stopped");
}

bool V4L2Encoder::encode_frame_dmabuf(int dmabuf_fd, size_t size, uint64_t timestamp) {
    if (!running_ || dmabuf_fd < 0) {
        return false;
    }

    frames_in_++;

    // Find a free input buffer slot
    int buf_idx = -1;
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        for (size_t i = 0; i < input_buffers_.size(); i++) {
            if (!input_buffers_[i].queued) {
                buf_idx = i;
                break;
            }
        }
    }

    if (buf_idx < 0) {
        // Try to dequeue a completed input buffer
        struct v4l2_buffer buf{};
        struct v4l2_plane planes[1]{};
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_DMABUF;
        buf.length = 1;
        buf.m.planes = planes;

        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno != EAGAIN) {
                LOG_ERROR("Failed to dequeue input buffer: ", strerror(errno));
            }
            dropped_frames_++;
            return false;
        }
        buf_idx = buf.index;
        std::lock_guard<std::mutex> lock(input_mutex_);
        input_buffers_[buf_idx].queued = false;
    }

    // Force keyframe if requested
    if (force_keyframe_.exchange(false)) {
        struct v4l2_control ctrl{};
        ctrl.id = V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME;
        ctrl.value = 1;
        ioctl(fd_, VIDIOC_S_CTRL, &ctrl);
    }

    // Queue the DMABUF directly (zero-copy)
    struct v4l2_buffer buf{};
    struct v4l2_plane planes[1]{};
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_DMABUF;
    buf.index = buf_idx;
    buf.length = 1;
    buf.m.planes = planes;
    planes[0].m.fd = dmabuf_fd;
    planes[0].bytesused = size;
    planes[0].length = size;
    buf.timestamp.tv_sec = timestamp / 1000000;
    buf.timestamp.tv_usec = timestamp % 1000000;

    if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        LOG_ERROR("Failed to queue DMABUF: ", strerror(errno));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        input_buffers_[buf_idx].queued = true;
    }

    return true;
}

bool V4L2Encoder::encode_frame_userptr(const uint8_t* data, size_t size, uint64_t timestamp) {
    if (!running_ || !data) {
        return false;
    }

    frames_in_++;

    // Find a free input buffer slot
    int buf_idx = -1;
    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        for (size_t i = 0; i < input_buffers_.size(); i++) {
            if (!input_buffers_[i].queued) {
                buf_idx = i;
                break;
            }
        }
    }

    if (buf_idx < 0) {
        // Try to dequeue a completed input buffer
        struct v4l2_buffer buf{};
        struct v4l2_plane planes[1]{};
        buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.length = 1;
        buf.m.planes = planes;

        if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
            if (errno != EAGAIN) {
                LOG_ERROR("Failed to dequeue input buffer: ", strerror(errno));
            }
            dropped_frames_++;
            return false;
        }
        buf_idx = buf.index;
        std::lock_guard<std::mutex> lock(input_mutex_);
        input_buffers_[buf_idx].queued = false;
    }

    // Verify buffer capacity
    if (size > input_buffers_[buf_idx].capacity) {
        LOG_ERROR("Frame size ", size, " exceeds buffer capacity ", input_buffers_[buf_idx].capacity);
        return false;
    }

    // Copy frame data into mapped buffer
    memcpy(input_buffers_[buf_idx].data, data, size);

    // Force keyframe if requested
    if (force_keyframe_.exchange(false)) {
        struct v4l2_control ctrl{};
        ctrl.id = V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME;
        ctrl.value = 1;
        ioctl(fd_, VIDIOC_S_CTRL, &ctrl);
    }

    // Queue MMAP buffer (using copy mode for software rotation)
    struct v4l2_buffer buf{};
    struct v4l2_plane planes[1]{};
    buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = buf_idx;
    buf.length = 1;
    buf.m.planes = planes;
    planes[0].bytesused = size;
    planes[0].length = input_buffers_[buf_idx].capacity;
    buf.timestamp.tv_sec = timestamp / 1000000;
    buf.timestamp.tv_usec = timestamp % 1000000;

    if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        LOG_ERROR("Failed to queue MMAP buffer: ", strerror(errno));
        return false;
    }

    {
        std::lock_guard<std::mutex> lock(input_mutex_);
        input_buffers_[buf_idx].queued = true;
    }

    return true;
}

void V4L2Encoder::set_output_callback(EncodedFrameCallback callback) {
    output_callback_ = callback;
}

void V4L2Encoder::force_keyframe() {
    force_keyframe_ = true;
}

void V4L2Encoder::output_thread_func() {
    LOG_DEBUG("Output thread started");

    while (running_) {
        struct pollfd pfd{};
        pfd.fd = fd_;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 100);  // 100ms timeout
        if (ret <= 0) {
            continue;
        }

        dequeue_output_buffer();
    }

    LOG_DEBUG("Output thread stopped");
}

void V4L2Encoder::dequeue_output_buffer() {
    struct v4l2_buffer buf{};
    struct v4l2_plane planes[1]{};
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.length = 1;
    buf.m.planes = planes;

    if (ioctl(fd_, VIDIOC_DQBUF, &buf) < 0) {
        if (errno != EAGAIN) {
            LOG_ERROR("Failed to dequeue output buffer: ", strerror(errno));
        }
        return;
    }

    int index = buf.index;
    size_t size = buf.m.planes[0].bytesused;
    
    if (size > 0 && output_callback_) {
        EncodedFrame frame;
        frame.metadata.timestamp_us = buf.timestamp.tv_sec * 1000000ULL + buf.timestamp.tv_usec;
        frame.metadata.sequence = frames_out_++;
        frame.metadata.width = config_.width;
        frame.metadata.height = config_.height;
        frame.metadata.size = size;
        frame.metadata.format = (config_.codec == Codec::H265) ? 3 : 2;
        
        // Check for keyframe (IDR)
        const uint8_t* data = static_cast<uint8_t*>(output_buffers_[index].data);
        frame.metadata.is_keyframe = (buf.flags & V4L2_BUF_FLAG_KEYFRAME) != 0;
        
        frame.data.resize(size);
        memcpy(frame.data.data(), data, size);
        
        bytes_out_ += size;
        
        output_callback_(frame);
    }

    // Re-queue the output buffer
    buf.m.planes[0].bytesused = 0;
    if (ioctl(fd_, VIDIOC_QBUF, &buf) < 0) {
        LOG_ERROR("Failed to re-queue output buffer: ", strerror(errno));
    }
}

V4L2Encoder::Stats V4L2Encoder::get_stats() const {
    return {
        frames_in_.load(),
        frames_out_.load(),
        bytes_out_.load(),
        dropped_frames_.load()
    };
}

} // namespace camera_daemon
