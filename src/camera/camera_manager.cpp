#include "camera_daemon/camera_manager.hpp"
#include "camera_daemon/logger.hpp"
#include <libcamera/controls.h>
#include <libcamera/orientation.h>
#include <sys/mman.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>

namespace camera_daemon {

CameraManager::CameraManager() = default;

CameraManager::~CameraManager() {
    stop();
    
    if (camera_) {
        camera_->release();
        camera_.reset();
    }
    
    if (cm_) {
        cm_->stop();
    }
}

bool CameraManager::initialize() {
    return initialize("");
}

bool CameraManager::initialize(const std::string& tuning_file) {
    LOG_INFO("Initializing camera manager");
    
    // Set tuning file for NoIR or other specialized camera modules
    if (!tuning_file.empty()) {
        // Resolve relative names to full path under standard libcamera IPA directory
        std::string tuning_path = tuning_file;
        if (tuning_file.find('/') == std::string::npos) {
            tuning_path = "/usr/share/libcamera/ipa/rpi/vc4/" + tuning_file;
        }
        LOG_INFO("Using tuning file: ", tuning_path);
        setenv("LIBCAMERA_RPI_TUNING_FILE", tuning_path.c_str(), 1);
    } else {
        // Clear any previously set tuning file to use libcamera default
        unsetenv("LIBCAMERA_RPI_TUNING_FILE");
        LOG_INFO("Using default tuning file");
    }
    
    cm_ = std::make_unique<libcamera::CameraManager>();
    
    int ret = cm_->start();
    if (ret != 0) {
        LOG_ERROR("Failed to start camera manager: ", ret);
        return false;
    }

    auto cameras = cm_->cameras();
    if (cameras.empty()) {
        LOG_ERROR("No cameras found");
        return false;
    }

    LOG_INFO("Found ", cameras.size(), " camera(s)");
    
    // Use the first available camera
    camera_ = cameras[0];
    
    ret = camera_->acquire();
    if (ret != 0) {
        LOG_ERROR("Failed to acquire camera: ", ret);
        return false;
    }

    LOG_INFO("Acquired camera: ", camera_->id());
    return true;
}

bool CameraManager::configure(const CameraConfig& config) {
    if (!camera_) {
        LOG_ERROR("Camera not initialized");
        return false;
    }

    config_ = config;
    
    LOG_INFO("Configuring camera: ", config.width, "x", config.height, "@", config.framerate, "fps");

    // Create configuration for video capture
    camera_config_ = camera_->generateConfiguration({libcamera::StreamRole::VideoRecording});
    if (!camera_config_) {
        LOG_ERROR("Failed to generate camera configuration");
        return false;
    }

    libcamera::StreamConfiguration& stream_config = camera_config_->at(0);
    stream_config.size.width = config.width;
    stream_config.size.height = config.height;
    
    // Request YUV420 pixel format (planar, efficient for JPEG and hardware encoding)
    stream_config.pixelFormat = libcamera::formats::YUV420;
    
    // Set buffer count
    stream_config.bufferCount = 6;

    // Compute ISP orientation from rotation + hflip + vflip (all free in hardware)
    // 180° rotation is equivalent to hflip + vflip
    bool effective_hflip = config.hflip;
    bool effective_vflip = config.vflip;
    if (config.rotation == 180) {
        effective_hflip = !effective_hflip;
        effective_vflip = !effective_vflip;
    }

    if (effective_hflip && effective_vflip) {
        camera_config_->orientation = libcamera::Orientation::Rotate180;
        LOG_INFO("ISP orientation: rotate 180°");
    } else if (effective_hflip) {
        camera_config_->orientation = libcamera::Orientation::Rotate0Mirror;
        LOG_INFO("ISP orientation: horizontal flip");
    } else if (effective_vflip) {
        camera_config_->orientation = libcamera::Orientation::Rotate180Mirror;
        LOG_INFO("ISP orientation: vertical flip");
    }
    // For 90°/270°, flips are applied by ISP before software rotation

    // Validate and apply — with format fallback for USB cameras
    libcamera::CameraConfiguration::Status status = camera_config_->validate();
    if (status == libcamera::CameraConfiguration::Invalid) {
        LOG_ERROR("Camera configuration invalid");
        return false;
    }

    // Check if pixel format was adjusted away from YUV420 (common with USB cameras)
    if (stream_config.pixelFormat != libcamera::formats::YUV420) {
        LOG_WARN("Camera does not support YUV420 at requested resolution/framerate");
        LOG_INFO("Adjusted to: ", stream_config.pixelFormat.toString(), " ",
                 stream_config.size.width, "x", stream_config.size.height);

        // Try YUYV — most USB cameras support it in raw; we'll convert to YUV420
        stream_config.size.width = config.width;
        stream_config.size.height = config.height;
        stream_config.pixelFormat = libcamera::formats::YUYV;
        status = camera_config_->validate();

        if (status != libcamera::CameraConfiguration::Invalid &&
            stream_config.pixelFormat == libcamera::formats::YUYV) {
            LOG_INFO("Using YUYV: ", stream_config.size.width, "x",
                     stream_config.size.height, " (will convert to YUV420)");
        } else {
            // Fall back to MJPEG — available on virtually all USB cameras at high res
            stream_config.size.width = config.width;
            stream_config.size.height = config.height;
            stream_config.pixelFormat = libcamera::formats::MJPEG;
            status = camera_config_->validate();
            if (status == libcamera::CameraConfiguration::Invalid) {
                LOG_ERROR("Camera supports no usable pixel format");
                return false;
            }
            LOG_INFO("Using MJPEG: ", stream_config.size.width, "x",
                     stream_config.size.height, " (will decode to YUV420)");
        }
    }

    if (status == libcamera::CameraConfiguration::Adjusted) {
        LOG_WARN("Camera configuration adjusted");
        LOG_INFO("Final: ", stream_config.pixelFormat.toString(), " ",
                 stream_config.size.width, "x", stream_config.size.height);
    }

    // Update config with actual negotiated dimensions
    config_.width = stream_config.size.width;
    config_.height = stream_config.size.height;

    // Record actual pixel format for pipeline to handle conversion
    auto pf = stream_config.pixelFormat;
    if (pf == libcamera::formats::YUV420)      actual_pixel_format_ = PIXFMT_YUV420;
    else if (pf == libcamera::formats::YUYV)   actual_pixel_format_ = PIXFMT_YUYV;
    else if (pf == libcamera::formats::MJPEG)  actual_pixel_format_ = PIXFMT_MJPEG;
    else if (pf == libcamera::formats::NV12)   actual_pixel_format_ = PIXFMT_NV12;
    else {
        LOG_WARN("Unknown pixel format: ", pf.toString(), ", assuming YUV420");
        actual_pixel_format_ = PIXFMT_YUV420;
    }

    int ret = camera_->configure(camera_config_.get());
    if (ret != 0) {
        LOG_ERROR("Failed to configure camera: ", ret);
        return false;
    }

    stream_ = stream_config.stream();
    
    // Allocate buffers
    allocator_ = std::make_unique<libcamera::FrameBufferAllocator>(camera_);
    ret = allocator_->allocate(stream_);
    if (ret < 0) {
        LOG_ERROR("Failed to allocate buffers: ", ret);
        return false;
    }

    LOG_INFO("Allocated ", allocator_->buffers(stream_).size(), " buffers");

    // Create requests
    const std::vector<std::unique_ptr<libcamera::FrameBuffer>>& buffers = allocator_->buffers(stream_);
    for (const auto& buffer : buffers) {
        std::unique_ptr<libcamera::Request> request = camera_->createRequest();
        if (!request) {
            LOG_ERROR("Failed to create request");
            return false;
        }

        ret = request->addBuffer(stream_, buffer.get());
        if (ret != 0) {
            LOG_ERROR("Failed to add buffer to request");
            return false;
        }

        requests_.push_back(std::move(request));
    }

    // Initialize pending controls with camera's control map
    pending_controls_ = libcamera::ControlList(camera_->controls());

    LOG_INFO("Camera configured successfully");
    return true;
}

bool CameraManager::start() {
    if (!camera_ || !camera_config_) {
        LOG_ERROR("Camera not configured");
        return false;
    }

    if (running_) {
        LOG_WARN("Camera already running");
        return true;
    }

    LOG_INFO("Starting camera capture");

    // Set up frame rate control
    libcamera::ControlList controls;
    int64_t frame_duration = 1000000 / config_.framerate;  // microseconds
    controls.set(libcamera::controls::FrameDurationLimits, 
                 libcamera::Span<const int64_t, 2>({frame_duration, frame_duration}));

    // Set ScalerCrop to the full sensor area to avoid 16:9 cropping
    // VideoRecording stream role applies digital crop, but we want full FOV
    const libcamera::ControlInfoMap& controlInfo = camera_->controls();
    auto it = controlInfo.find(&libcamera::controls::ScalerCrop);
    if (it != controlInfo.end()) {
        libcamera::Rectangle fullSensor = it->second.max().get<libcamera::Rectangle>();
        LOG_INFO("Setting ScalerCrop to full sensor: ", fullSensor.x, ",", fullSensor.y, " ",
                 fullSensor.width, "x", fullSensor.height);
        controls.set(libcamera::controls::ScalerCrop, fullSensor);
        scaler_crop_ = fullSensor;  // Store for use when requeuing requests
    } else {
        LOG_WARN("ScalerCrop control not available");
        scaler_crop_.reset();
    }

    // Connect request completed signal
    camera_->requestCompleted.connect(this, &CameraManager::request_complete);

    int ret = camera_->start(&controls);
    if (ret != 0) {
        LOG_ERROR("Failed to start camera: ", ret);
        return false;
    }

    running_ = true;
    frame_sequence_ = 0;

    // Queue all requests
    for (auto& request : requests_) {
        ret = camera_->queueRequest(request.get());
        if (ret != 0) {
            LOG_ERROR("Failed to queue request: ", ret);
            stop();
            return false;
        }
    }

    LOG_INFO("Camera capture started");
    return true;
}

void CameraManager::stop() {
    if (!running_) {
        return;
    }

    LOG_INFO("Stopping camera capture");
    running_ = false;

    if (camera_) {
        camera_->stop();
        camera_->requestCompleted.disconnect(this, &CameraManager::request_complete);
    }

    // Clear completed requests queue
    {
        std::lock_guard<std::mutex> lock(mutex_);
        while (!completed_requests_.empty()) {
            completed_requests_.pop();
        }
    }

    LOG_INFO("Camera capture stopped");
}

void CameraManager::set_frame_callback(FrameCallback callback) {
    std::lock_guard<std::mutex> lock(mutex_);
    frame_callback_ = callback;
}

void CameraManager::request_complete(libcamera::Request* request) {
    if (!running_) {
        return;
    }

    if (request->status() == libcamera::Request::RequestCancelled) {
        return;
    }

    // Get the buffer
    const libcamera::Request::BufferMap& buffers = request->buffers();
    auto it = buffers.find(stream_);
    if (it == buffers.end()) {
        LOG_ERROR("No buffer in completed request");
        return;
    }

    libcamera::FrameBuffer* buffer = it->second;
    const libcamera::ControlList& metadata = request->metadata();

    // Process the frame
    process_frame(buffer, metadata);

    // Reuse the request
    request->reuse(libcamera::Request::ReuseBuffers);
    
    // Apply any pending controls
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!pending_controls_.empty()) {
            for (const auto& [id, value] : pending_controls_)
                request->controls().set(id, value);
            pending_controls_.clear();
        }
    }
    
    // Note: ScalerCrop is set at camera start and persists
    int ret = camera_->queueRequest(request);
    if (ret < 0) {
        LOG_ERROR("Failed to queue request: ", ret);
    }
}

void CameraManager::process_frame(libcamera::FrameBuffer* buffer, const libcamera::ControlList& metadata) {
    // Get frame metadata
    const libcamera::FrameMetadata& frame_meta = buffer->metadata();
    
    // Get the actual stride from stream configuration
    const libcamera::StreamConfiguration& stream_config = camera_config_->at(0);
    uint32_t actual_stride = stream_config.stride;
    
    FrameMetadata fm;
    fm.timestamp_us = frame_meta.timestamp / 1000;  // Convert from nanoseconds
    fm.sequence = frame_sequence_++;
    fm.width = stream_config.size.width;
    fm.height = stream_config.size.height;
    fm.stride = actual_stride;  // Use actual stride from libcamera
    fm.size = 0;
    fm.is_keyframe = false;
    fm.format = actual_pixel_format_;
    
    LOG_DEBUG("Frame ", fm.sequence, ": ", fm.width, "x", fm.height,
              " stride=", actual_stride, " fmt=", static_cast<int>(fm.format));

    // Calculate total size and map the buffer
    const std::vector<libcamera::FrameBuffer::Plane>& planes = buffer->planes();
    
    void* mapped_mem = nullptr;
    size_t mapped_size = 0;
    
    // Pass DMA-BUF fd for zero-copy encoding
    fm.dmabuf_fd = planes[0].fd.get();
    
    if (actual_pixel_format_ == PIXFMT_YUV420 && planes.size() >= 3) {
        // YUV420: three separate planes (Y, U, V)
        mapped_size = planes[2].offset + planes[2].length;  // Last plane end
        
        mapped_mem = mmap(nullptr, mapped_size, PROT_READ, MAP_SHARED, planes[0].fd.get(), 0);
        if (mapped_mem == MAP_FAILED) {
            LOG_ERROR("Failed to mmap buffer");
            return;
        }
        
        fm.size = mapped_size;
    } else {
        // Single plane: YUYV (packed), MJPEG (compressed), or NV12
        const auto& plane = planes[0];
        mapped_size = plane.length + plane.offset;
        
        mapped_mem = mmap(nullptr, mapped_size, PROT_READ, MAP_SHARED, plane.fd.get(), 0);
        if (mapped_mem == MAP_FAILED) {
            LOG_ERROR("Failed to mmap buffer plane");
            return;
        }
        
        if (actual_pixel_format_ == PIXFMT_MJPEG) {
            // MJPEG: actual compressed data may be smaller than buffer capacity
            const auto& meta_planes = frame_meta.planes();
            fm.size = meta_planes.empty() ? plane.length : meta_planes[0].bytesused;
        } else {
            fm.size = plane.length;
        }
    }

    // Call the callback with mapped memory
    // Note: dmabuf_fd is valid during callback for zero-copy encoding
    FrameCallback callback;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        callback = frame_callback_;
    }

    if (callback) {
        const uint8_t* frame_ptr = static_cast<const uint8_t*>(mapped_mem);
        callback(fm, frame_ptr, fm.size);
    }
    
    // Unmap after callback completes
    munmap(mapped_mem, mapped_size);
}

bool CameraManager::set_control(const std::string& name, int64_t value) {
    if (!camera_ || !running_) {
        LOG_WARN("Cannot set control - camera not running");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    
    if (name == "exposure" || name == "exposure_time") {
        // Exposure time in microseconds
        pending_controls_.set(libcamera::controls::ExposureTime, static_cast<int32_t>(value));
        LOG_INFO("Set ExposureTime: ", value, " us");
    } else if (name == "ae_enable" || name == "auto_exposure") {
        pending_controls_.set(libcamera::controls::AeEnable, value != 0);
        LOG_INFO("Set AeEnable: ", (value != 0 ? "true" : "false"));
    } else if (name == "awb_enable" || name == "auto_white_balance") {
        pending_controls_.set(libcamera::controls::AwbEnable, value != 0);
        LOG_INFO("Set AwbEnable: ", (value != 0 ? "true" : "false"));
    } else if (name == "ae_metering" || name == "metering_mode") {
        // 0=Centre, 1=Spot, 2=Matrix, 3=Custom
        pending_controls_.set(libcamera::controls::AeMeteringMode, static_cast<int32_t>(value));
        LOG_INFO("Set AeMeteringMode: ", value);
    } else if (name == "ae_constraint" || name == "exposure_constraint") {
        // 0=Normal, 1=Highlight, 2=Shadows, 3=Custom
        pending_controls_.set(libcamera::controls::AeConstraintMode, static_cast<int32_t>(value));
        LOG_INFO("Set AeConstraintMode: ", value);
    } else if (name == "ae_exposure" || name == "exposure_value") {
        // EV compensation
        pending_controls_.set(libcamera::controls::ExposureValue, static_cast<float>(value) / 10.0f);
        LOG_INFO("Set ExposureValue: ", value / 10.0f);
    } else if (name == "sharpness") {
        pending_controls_.set(libcamera::controls::Sharpness, static_cast<float>(value) / 100.0f);
        LOG_INFO("Set Sharpness: ", value / 100.0f);
    } else if (name == "saturation") {
        pending_controls_.set(libcamera::controls::Saturation, static_cast<float>(value) / 100.0f);
        LOG_INFO("Set Saturation: ", value / 100.0f);
    } else {
        LOG_WARN("Unknown integer control: ", name);
        return false;
    }

    return true;
}

bool CameraManager::set_control(const std::string& name, float value) {
    if (!camera_ || !running_) {
        LOG_WARN("Cannot set control - camera not running");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    
    if (name == "gain" || name == "analogue_gain") {
        pending_controls_.set(libcamera::controls::AnalogueGain, value);
        LOG_INFO("Set AnalogueGain: ", value);
    } else if (name == "brightness") {
        // Range: -1.0 to 1.0
        pending_controls_.set(libcamera::controls::Brightness, value);
        LOG_INFO("Set Brightness: ", value);
    } else if (name == "contrast") {
        // Range: 0.0 to 32.0, 1.0 is normal
        pending_controls_.set(libcamera::controls::Contrast, value);
        LOG_INFO("Set Contrast: ", value);
    } else if (name == "sharpness") {
        pending_controls_.set(libcamera::controls::Sharpness, value);
        LOG_INFO("Set Sharpness: ", value);
    } else if (name == "saturation") {
        pending_controls_.set(libcamera::controls::Saturation, value);
        LOG_INFO("Set Saturation: ", value);
    } else if (name == "exposure_value" || name == "ev") {
        pending_controls_.set(libcamera::controls::ExposureValue, value);
        LOG_INFO("Set ExposureValue: ", value);
    } else {
        LOG_WARN("Unknown float control: ", name);
        return false;
    }

    return true;
}

bool CameraManager::set_control(const std::string& name, bool value) {
    if (!camera_ || !running_) {
        LOG_WARN("Cannot set control - camera not running");
        return false;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    
    if (name == "ae_enable" || name == "auto_exposure") {
        pending_controls_.set(libcamera::controls::AeEnable, value);
        LOG_INFO("Set AeEnable: ", (value ? "true" : "false"));
    } else if (name == "awb_enable" || name == "auto_white_balance") {
        pending_controls_.set(libcamera::controls::AwbEnable, value);
        LOG_INFO("Set AwbEnable: ", (value ? "true" : "false"));
    } else {
        LOG_WARN("Unknown boolean control: ", name);
        return false;
    }

    return true;
}

} // namespace camera_daemon
