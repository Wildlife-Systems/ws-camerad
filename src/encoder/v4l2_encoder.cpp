#include "camera_daemon/v4l2_encoder.hpp"
#include "camera_daemon/logger.hpp"
#include <fcntl.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <linux/videodev2.h>
#include <cstring>
#include <poll.h>
#include <cstdlib>
#include <filesystem>
#include <vector>
#include <algorithm>
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <gst/app/gstappsink.h>
#include <gst/video/video-event.h>

namespace camera_daemon {

namespace {
    constexpr int NUM_INPUT_BUFFERS = 4;
    constexpr int NUM_OUTPUT_BUFFERS = 4;

    bool supports_codec_capture_format(int fd, V4L2Encoder::Codec codec) {
        const uint32_t wanted = (codec == V4L2Encoder::Codec::H265) ? V4L2_PIX_FMT_HEVC : V4L2_PIX_FMT_H264;

        struct v4l2_fmtdesc fmt{};
        fmt.type = V4L2_BUF_TYPE_VIDEO_CAPTURE_MPLANE;
        for (fmt.index = 0; ioctl(fd, VIDIOC_ENUM_FMT, &fmt) == 0; ++fmt.index) {
            if (fmt.pixelformat == wanted) {
                return true;
            }
        }
        return false;
    }

    bool probe_encoder_device(const std::string& path, V4L2Encoder::Codec codec, std::string* reason = nullptr) {
        int fd = open(path.c_str(), O_RDWR | O_NONBLOCK);
        if (fd < 0) {
            if (reason) *reason = std::string("open failed: ") + strerror(errno);
            return false;
        }

        struct v4l2_capability cap{};
        if (ioctl(fd, VIDIOC_QUERYCAP, &cap) < 0) {
            if (reason) *reason = std::string("VIDIOC_QUERYCAP failed: ") + strerror(errno);
            close(fd);
            return false;
        }

        if (!(cap.capabilities & V4L2_CAP_VIDEO_M2M_MPLANE)) {
            if (reason) *reason = "not V4L2_CAP_VIDEO_M2M_MPLANE";
            close(fd);
            return false;
        }

        if (!supports_codec_capture_format(fd, codec)) {
            if (reason) *reason = "M2M present but requested codec capture format not supported";
            close(fd);
            return false;
        }

        close(fd);
        return true;
    }

    std::vector<std::string> candidate_encoder_devices() {
        std::vector<std::string> devices;

        // Keep legacy default first for compatibility with older Pi setups.
        devices.push_back("/dev/video11");

        try {
            for (const auto& entry : std::filesystem::directory_iterator("/dev")) {
                const auto name = entry.path().filename().string();
                if (name.rfind("video", 0) == 0) {
                    devices.push_back(entry.path().string());
                }
            }
        } catch (...) {
            // Ignore /dev scan failures and rely on defaults.
        }

        std::sort(devices.begin(), devices.end());
        devices.erase(std::unique(devices.begin(), devices.end()), devices.end());
        return devices;
    }

    bool ensure_gstreamer_initialized() {
        static std::once_flag gst_once;
        static bool ok = false;
        std::call_once(gst_once, []() {
            gst_init(nullptr, nullptr);
            ok = true;
        });
        return ok;
    }
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

    if (!open_device()) {
        LOG_WARN("Falling back to software H.264 encoding (GStreamer)");
        backend_ = Backend::SoftwareGStreamer;
        // Software path always operates on CPU-accessible frames.
        config_.codec = Codec::H264;
        config_.use_userptr = true;
        return initialize_software_encoder();
    }

    backend_ = Backend::HardwareV4L2;
    if (!setup_output_format()) return false;  // Output (encoded) first
    if (!setup_input_format()) return false;   // Input (raw) second
    if (!setup_controls()) return false;
    if (!allocate_buffers()) return false;

    LOG_INFO("V4L2 encoder initialized successfully");
    return true;
}

bool V4L2Encoder::open_device() {
    std::vector<std::string> candidates;

    const char* forced = std::getenv("WS_CAMERAD_ENCODER_DEVICE");
    if (forced && *forced) {
        candidates.emplace_back(forced);
    }

    const auto discovered = candidate_encoder_devices();
    candidates.insert(candidates.end(), discovered.begin(), discovered.end());

    std::sort(candidates.begin(), candidates.end());
    candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());

    std::string last_reason;
    for (const auto& device : candidates) {
        std::string reason;
        if (!probe_encoder_device(device, config_.codec, &reason)) {
            last_reason = reason;
            continue;
        }

        // Retry opening chosen device - previous user may still be releasing resources
        for (int attempt = 0; attempt < 5; ++attempt) {
            fd_ = open(device.c_str(), O_RDWR | O_NONBLOCK);
            if (fd_ >= 0) {
                LOG_INFO("Using V4L2 encoder device: ", device);
                break;
            }
            if (errno == EBUSY && attempt < 4) {
                usleep(10000);  // 10ms
                continue;
            }
        }

        if (fd_ >= 0) {
            break;
        }
    }

    if (fd_ < 0) {
        LOG_ERROR("No compatible V4L2 M2M encoder device found for requested codec. ",
                  "This platform may not provide hardware video encoding. Last probe reason: ",
                  last_reason.empty() ? "none" : last_reason,
                  ". You can force a device with WS_CAMERAD_ENCODER_DEVICE=/dev/videoX");
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

    LOG_DEBUG("Encoder card: ", reinterpret_cast<char*>(cap.card));
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

    if (backend_ == Backend::SoftwareGStreamer) {
        auto* pipeline = static_cast<GstElement*>(sw_pipeline_);
        if (!pipeline) {
            LOG_ERROR("Software encoder pipeline is not initialized");
            return false;
        }
        GstStateChangeReturn ret = gst_element_set_state(pipeline, GST_STATE_PLAYING);
        if (ret == GST_STATE_CHANGE_FAILURE) {
            LOG_ERROR("Failed to set software encoder pipeline to PLAYING");
            return false;
        }
        running_ = true;
        LOG_INFO("Software encoder started");
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
    if (backend_ == Backend::SoftwareGStreamer) {
        if (!sw_pipeline_) {
            running_ = false;
            return;
        }
        LOG_INFO("Stopping software encoder");
        running_ = false;
        cleanup_software_encoder();
        return;
    }

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

    if (backend_ == Backend::SoftwareGStreamer) {
        void* mapped = mmap(nullptr, size, PROT_READ, MAP_SHARED, dmabuf_fd, 0);
        if (mapped == MAP_FAILED) {
            LOG_ERROR("Software fallback failed to mmap dmabuf: ", strerror(errno));
            dropped_frames_++;
            return false;
        }
        bool ok = encode_frame_software(static_cast<const uint8_t*>(mapped), size, timestamp);
        munmap(mapped, size);
        return ok;
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

    if (backend_ == Backend::SoftwareGStreamer) {
        return encode_frame_software(data, size, timestamp);
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

bool V4L2Encoder::initialize_software_encoder() {
    if (!ensure_gstreamer_initialized()) {
        LOG_ERROR("Failed to initialize GStreamer for software encoder fallback");
        return false;
    }

    const std::vector<std::string> encoders = {"x264enc", "openh264enc", "avenc_h264"};
    GstElement* encoder = nullptr;
    std::string encoder_name;
    for (const auto& name : encoders) {
        encoder = gst_element_factory_make(name.c_str(), "swenc");
        if (encoder) {
            encoder_name = name;
            break;
        }
    }

    if (!encoder) {
        LOG_ERROR("No software H.264 encoder plugin found (tried x264enc, openh264enc, avenc_h264)");
        return false;
    }

    GstElement* pipeline = gst_pipeline_new("ws-camerad-sw-encoder");
    GstElement* appsrc = gst_element_factory_make("appsrc", "src");
    GstElement* parse = gst_element_factory_make("h264parse", "parse");
    GstElement* appsink = gst_element_factory_make("appsink", "sink");
    if (!pipeline || !appsrc || !parse || !appsink) {
        if (pipeline) gst_object_unref(pipeline);
        if (appsrc) gst_object_unref(appsrc);
        if (encoder) gst_object_unref(encoder);
        if (parse) gst_object_unref(parse);
        if (appsink) gst_object_unref(appsink);
        LOG_ERROR("Failed to create software encoder pipeline elements");
        return false;
    }

    GstCaps* caps = gst_caps_new_simple(
        "video/x-raw",
        "format", G_TYPE_STRING, "I420",
        "width", G_TYPE_INT, static_cast<int>(config_.width),
        "height", G_TYPE_INT, static_cast<int>(config_.height),
        "framerate", GST_TYPE_FRACTION, static_cast<int>(config_.framerate), 1,
        nullptr);
    g_object_set(appsrc,
                 "is-live", TRUE,
                 "format", GST_FORMAT_TIME,
                 "do-timestamp", FALSE,
                 "block", FALSE,
                 "caps", caps,
                 nullptr);
    gst_caps_unref(caps);

    g_object_set(appsink,
                 "emit-signals", FALSE,
                 "sync", FALSE,
                 "drop", TRUE,
                 "max-buffers", 8,
                 nullptr);

    if (encoder_name == "x264enc") {
        g_object_set(encoder,
                     "tune", 0x00000004,  // zerolatency
                     "speed-preset", 1,   // ultrafast
                     "bitrate", static_cast<int>(config_.bitrate / 1000),
                     "key-int-max", static_cast<int>(config_.keyframe_interval),
                     "byte-stream", TRUE,
                     nullptr);
    } else if (encoder_name == "openh264enc") {
        g_object_set(encoder,
                     "bitrate", static_cast<int>(config_.bitrate),
                     "gop-size", static_cast<int>(config_.keyframe_interval),
                     nullptr);
    } else {
        // avenc_h264 typically expects bitrate in bits/sec.
        g_object_set(encoder,
                     "bitrate", static_cast<int>(config_.bitrate),
                     "gop_size", static_cast<int>(config_.keyframe_interval),
                     nullptr);
    }

    gst_bin_add_many(GST_BIN(pipeline), appsrc, encoder, parse, appsink, nullptr);
    if (!gst_element_link_many(appsrc, encoder, parse, appsink, nullptr)) {
        gst_object_unref(pipeline);
        LOG_ERROR("Failed to link software encoder pipeline");
        return false;
    }

    sw_pipeline_ = pipeline;
    sw_appsrc_ = appsrc;
    sw_appsink_ = appsink;
    LOG_INFO("Software encoder fallback initialized with ", encoder_name);
    return true;
}

bool V4L2Encoder::encode_frame_software(const uint8_t* data, size_t size, uint64_t timestamp) {
    auto* appsrc = static_cast<GstAppSrc*>(sw_appsrc_);
    if (!appsrc || !data) {
        return false;
    }

    frames_in_++;

    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    if (!buffer) {
        dropped_frames_++;
        return false;
    }

    GstMapInfo map;
    if (!gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        gst_buffer_unref(buffer);
        dropped_frames_++;
        return false;
    }
    memcpy(map.data, data, size);
    gst_buffer_unmap(buffer, &map);

    GST_BUFFER_PTS(buffer) = timestamp * 1000ULL;
    GST_BUFFER_DTS(buffer) = GST_BUFFER_PTS(buffer);
    GST_BUFFER_DURATION(buffer) = gst_util_uint64_scale_int(1, GST_SECOND, static_cast<int>(config_.framerate));

    if (force_keyframe_.exchange(false)) {
        GstPad* srcpad = gst_element_get_static_pad(GST_ELEMENT(appsrc), "src");
        if (srcpad) {
            GstEvent* ev = gst_video_event_new_upstream_force_key_unit(
                GST_BUFFER_PTS(buffer), TRUE, 0);
            gst_pad_send_event(srcpad, ev);
            gst_object_unref(srcpad);
        }
    }

    GstFlowReturn ret = gst_app_src_push_buffer(appsrc, buffer);
    if (ret != GST_FLOW_OK) {
        LOG_WARN("Software encoder push failed: ", ret);
        dropped_frames_++;
        return false;
    }

    drain_software_output();
    return true;
}

void V4L2Encoder::drain_software_output() {
    auto* appsink = static_cast<GstAppSink*>(sw_appsink_);
    if (!appsink) {
        return;
    }

    while (true) {
        GstSample* sample = gst_app_sink_try_pull_sample(appsink, 0);
        if (!sample) {
            break;
        }

        GstBuffer* buffer = gst_sample_get_buffer(sample);
        if (buffer && output_callback_) {
            GstMapInfo map;
            if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
                EncodedFrame frame;
                frame.metadata.timestamp_us = GST_BUFFER_PTS_IS_VALID(buffer) ? (GST_BUFFER_PTS(buffer) / 1000ULL) : 0;
                frame.metadata.sequence = frames_out_++;
                frame.metadata.width = config_.width;
                frame.metadata.height = config_.height;
                frame.metadata.size = map.size;
                frame.metadata.format = 2; // H.264
                frame.metadata.is_keyframe = !GST_BUFFER_FLAG_IS_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
                frame.data.assign(map.data, map.data + map.size);
                bytes_out_ += map.size;
                output_callback_(frame);
                gst_buffer_unmap(buffer, &map);
            }
        }

        gst_sample_unref(sample);
    }
}

void V4L2Encoder::cleanup_software_encoder() {
    auto* pipeline = static_cast<GstElement*>(sw_pipeline_);
    auto* appsrc = static_cast<GstAppSrc*>(sw_appsrc_);

    if (appsrc) {
        gst_app_src_end_of_stream(appsrc);
    }
    drain_software_output();

    if (pipeline) {
        gst_element_set_state(pipeline, GST_STATE_NULL);
        gst_object_unref(pipeline);
    }

    sw_pipeline_ = nullptr;
    sw_appsrc_ = nullptr;
    sw_appsink_ = nullptr;
    LOG_INFO("Software encoder stopped");
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
