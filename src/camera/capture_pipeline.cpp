#include "camera_daemon/capture_pipeline.hpp"
#include "camera_daemon/logger.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <set>

#if defined(__aarch64__) || defined(__ARM_NEON)
#include <arm_neon.h>
#define USE_NEON 1
#else
#define USE_NEON 0
#endif

namespace camera_daemon {

#if USE_NEON
// NEON-optimized YUV420 planar to BGR24 conversion
// Processes 16 pixels at a time using SIMD for maximum throughput
// ~8-10x faster than scalar version on ARM64
static void yuv420_to_bgr(const uint8_t* yuv, uint8_t* bgr, 
                          uint32_t width, uint32_t height, uint32_t stride) {
    const uint8_t* y_plane = yuv;
    const uint8_t* u_plane = yuv + stride * height;
    const uint8_t* v_plane = u_plane + (stride / 2) * (height / 2);
    
    uint32_t uv_stride = stride / 2;
    
    // BT.601 coefficients scaled by 64 for fixed-point math
    const int16x8_t v_coeff_r = vdupq_n_s16(90);
    const int16x8_t u_coeff_g = vdupq_n_s16(22);
    const int16x8_t v_coeff_g = vdupq_n_s16(46);
    const int16x8_t u_coeff_b = vdupq_n_s16(113);
    const int16x8_t const_128 = vdupq_n_s16(128);
    
    for (uint32_t row = 0; row < height; row++) {
        const uint8_t* y_row = y_plane + row * stride;
        const uint8_t* u_row = u_plane + (row / 2) * uv_stride;
        const uint8_t* v_row = v_plane + (row / 2) * uv_stride;
        uint8_t* bgr_row = bgr + row * width * 3;
        
        uint32_t col = 0;
        
        // Process 16 pixels at a time (two 8-pixel batches, better cache utilization)
        for (; col + 16 <= width; col += 16) {
            // First 8 pixels
            uint8x8_t y8_lo = vld1_u8(y_row + col);
            uint8x8_t y8_hi = vld1_u8(y_row + col + 8);
            
            // Load 8 U and V values for 16 pixels
            uint8x8_t u8_raw = vld1_u8(u_row + col / 2);
            uint8x8_t v8_raw = vld1_u8(v_row + col / 2);
            
            // Duplicate each U/V value for 2 adjacent pixels
            uint8x8x2_t u_dup = vzip_u8(u8_raw, u8_raw);
            uint8x8x2_t v_dup = vzip_u8(v8_raw, v8_raw);
            
            // Process first 8 pixels
            {
                int16x8_t y16 = vreinterpretq_s16_u16(vmovl_u8(y8_lo));
                int16x8_t u16 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(u_dup.val[0])), const_128);
                int16x8_t v16 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(v_dup.val[0])), const_128);
                
                int16x8_t r16 = vaddq_s16(y16, vshrq_n_s16(vmulq_s16(v16, v_coeff_r), 6));
                int16x8_t g16 = vsubq_s16(y16, vshrq_n_s16(vaddq_s16(vmulq_s16(u16, u_coeff_g), 
                                                                      vmulq_s16(v16, v_coeff_g)), 6));
                int16x8_t b16 = vaddq_s16(y16, vshrq_n_s16(vmulq_s16(u16, u_coeff_b), 6));
                
                uint8x8x3_t bgr_out;
                bgr_out.val[0] = vqmovun_s16(b16);
                bgr_out.val[1] = vqmovun_s16(g16);
                bgr_out.val[2] = vqmovun_s16(r16);
                vst3_u8(bgr_row + col * 3, bgr_out);
            }
            
            // Process second 8 pixels
            {
                int16x8_t y16 = vreinterpretq_s16_u16(vmovl_u8(y8_hi));
                int16x8_t u16 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(u_dup.val[1])), const_128);
                int16x8_t v16 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(v_dup.val[1])), const_128);
                
                int16x8_t r16 = vaddq_s16(y16, vshrq_n_s16(vmulq_s16(v16, v_coeff_r), 6));
                int16x8_t g16 = vsubq_s16(y16, vshrq_n_s16(vaddq_s16(vmulq_s16(u16, u_coeff_g), 
                                                                      vmulq_s16(v16, v_coeff_g)), 6));
                int16x8_t b16 = vaddq_s16(y16, vshrq_n_s16(vmulq_s16(u16, u_coeff_b), 6));
                
                uint8x8x3_t bgr_out;
                bgr_out.val[0] = vqmovun_s16(b16);
                bgr_out.val[1] = vqmovun_s16(g16);
                bgr_out.val[2] = vqmovun_s16(r16);
                vst3_u8(bgr_row + (col + 8) * 3, bgr_out);
            }
        }
        
        // Handle remaining 8 pixels if any
        if (col + 8 <= width) {
            uint8x8_t y8 = vld1_u8(y_row + col);
            int16x8_t y16 = vreinterpretq_s16_u16(vmovl_u8(y8));
            
            uint8x8_t u4 = vld1_u8(u_row + col / 2);
            uint8x8_t v4 = vld1_u8(v_row + col / 2);
            
            uint8x8x2_t u_dup = vzip_u8(u4, u4);
            uint8x8x2_t v_dup = vzip_u8(v4, v4);
            
            int16x8_t u16 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(u_dup.val[0])), const_128);
            int16x8_t v16 = vsubq_s16(vreinterpretq_s16_u16(vmovl_u8(v_dup.val[0])), const_128);
            
            int16x8_t r16 = vaddq_s16(y16, vshrq_n_s16(vmulq_s16(v16, v_coeff_r), 6));
            int16x8_t g16 = vsubq_s16(y16, vshrq_n_s16(vaddq_s16(vmulq_s16(u16, u_coeff_g), 
                                                                  vmulq_s16(v16, v_coeff_g)), 6));
            int16x8_t b16 = vaddq_s16(y16, vshrq_n_s16(vmulq_s16(u16, u_coeff_b), 6));
            
            uint8x8x3_t bgr_out;
            bgr_out.val[0] = vqmovun_s16(b16);
            bgr_out.val[1] = vqmovun_s16(g16);
            bgr_out.val[2] = vqmovun_s16(r16);
            vst3_u8(bgr_row + col * 3, bgr_out);
            col += 8;
        }
        
        // Handle remaining pixels (scalar)
        for (; col < width; col++) {
            int y = y_row[col];
            int u = u_row[col / 2] - 128;
            int v = v_row[col / 2] - 128;
            
            int r = y + ((90 * v) >> 6);
            int g = y - ((22 * u + 46 * v) >> 6);
            int b = y + ((113 * u) >> 6);
            
            r = std::max(0, std::min(255, r));
            g = std::max(0, std::min(255, g));
            b = std::max(0, std::min(255, b));
            
            size_t idx = col * 3;
            bgr_row[idx + 0] = static_cast<uint8_t>(b);
            bgr_row[idx + 1] = static_cast<uint8_t>(g);
            bgr_row[idx + 2] = static_cast<uint8_t>(r);
        }
    }
}

#else
// Scalar fallback for non-ARM platforms
static void yuv420_to_bgr(const uint8_t* yuv, uint8_t* bgr, 
                          uint32_t width, uint32_t height, uint32_t stride) {
    const uint8_t* y_plane = yuv;
    const uint8_t* u_plane = yuv + stride * height;
    const uint8_t* v_plane = u_plane + (stride / 2) * (height / 2);
    
    uint32_t uv_stride = stride / 2;
    
    for (uint32_t row = 0; row < height; row++) {
        for (uint32_t col = 0; col < width; col++) {
            int y = y_plane[row * stride + col];
            int u = u_plane[(row / 2) * uv_stride + (col / 2)] - 128;
            int v = v_plane[(row / 2) * uv_stride + (col / 2)] - 128;
            
            // BT.601 conversion
            int r = y + ((359 * v) >> 8);
            int g = y - ((88 * u + 183 * v) >> 8);
            int b = y + ((454 * u) >> 8);
            
            r = std::max(0, std::min(255, r));
            g = std::max(0, std::min(255, g));
            b = std::max(0, std::min(255, b));
            
            size_t bgr_idx = (row * width + col) * 3;
            bgr[bgr_idx + 0] = static_cast<uint8_t>(b);
            bgr[bgr_idx + 1] = static_cast<uint8_t>(g);
            bgr[bgr_idx + 2] = static_cast<uint8_t>(r);
        }
    }
}
#endif

CapturePipeline::CapturePipeline(const DaemonConfig& config)
    : config_(config) {
}

CapturePipeline::~CapturePipeline() {
    stop();
}

bool CapturePipeline::initialize() {
    LOG_INFO("Initializing capture pipeline");

    // Create camera manager
    camera_manager_ = std::make_unique<CameraManager>();
    if (!camera_manager_->initialize(config_.camera.tuning_file)) {
        LOG_ERROR("Failed to initialize camera manager");
        return false;
    }

    if (!camera_manager_->configure(config_.camera)) {
        LOG_ERROR("Failed to configure camera");
        return false;
    }

    // Create encoder
    encoder_ = std::make_unique<V4L2Encoder>();
    V4L2Encoder::Config enc_config;

    // For 90°/270° rotation, the output dimensions are swapped
    bool needs_sw_rotation = (config_.camera.rotation == 90 || config_.camera.rotation == 270);
    uint32_t output_width = needs_sw_rotation ? config_.camera.height : config_.camera.width;
    uint32_t output_height = needs_sw_rotation ? config_.camera.width : config_.camera.height;

    enc_config.width = output_width;
    enc_config.height = output_height;
    enc_config.framerate = config_.camera.framerate;
    enc_config.bitrate = config_.camera.bitrate;
    enc_config.keyframe_interval = config_.camera.keyframe_interval;
    enc_config.use_userptr = needs_sw_rotation;  // Software-rotated frames aren't in DMABUFs
    
    if (!encoder_->initialize(enc_config)) {
        LOG_ERROR("Failed to initialize encoder");
        return false;
    }

    // Create ring buffer for clips
    ring_buffer_ = std::make_unique<EncodedRingBuffer>(
        config_.ring_buffer_seconds,
        config_.camera.framerate
    );

    // Create shared memory publisher for raw frames (optional)
    if (config_.enable_raw_sharing) {
        frame_publisher_ = std::make_unique<FramePublisher>(
            config_.shm_name,
            output_width,
            output_height,
            PIXFMT_YUV420
        );
    }

    // Create shared memory publisher for BGR frames (optional, for OpenCV consumers)
    if (config_.enable_bgr_sharing) {
        bgr_frame_publisher_ = std::make_unique<FramePublisher>(
            config_.bgr_shm_name,
            output_width,
            output_height,
            PIXFMT_BGR24
        );
        // Pre-allocate BGR buffer
        bgr_buffer_.resize(output_width * output_height * 3);
    }

    // Create still capture
    StillCapture::Config still_config;
    still_config.output_dir = config_.stills_dir;
    still_config.jpeg_quality = config_.camera.jpeg_quality;
    still_config.width = output_width;
    still_config.height = output_height;
    still_capture_ = std::make_unique<StillCapture>(still_config);

    // Create clip extractor
    ClipExtractor::Config clip_config;
    clip_config.output_dir = config_.clips_dir;
    clip_config.pre_event_seconds = config_.ring_buffer_seconds;
    clip_config.post_event_seconds = config_.post_event_seconds;

    // Create audio reader (optional, for muxing audio into clips)
    if (config_.enable_audio) {
        AudioReader::Config audio_config;
        audio_config.shm_name = config_.audio_shm_name;
        audio_config.buffer_seconds = config_.audio_buffer_seconds;
        audio_reader_ = std::make_unique<AudioReader>(audio_config);
        LOG_INFO("Audio reader created, shm='", config_.audio_shm_name,
                 "', buffer=", config_.audio_buffer_seconds, "s",
                 ", rtsp_audio=", config_.enable_rtsp_audio);
    } else {
        LOG_INFO("Audio disabled");
    }

    clip_extractor_ = std::make_unique<ClipExtractor>(
        clip_config, *ring_buffer_, audio_reader_.get());

    // Create RTSP server (optional)
    if (config_.enable_rtsp) {
        RTSPServer::Config rtsp_config;
        rtsp_config.port = config_.rtsp_port;
        rtsp_config.width = output_width;
        rtsp_config.height = output_height;
        rtsp_config.framerate = config_.camera.framerate;
        rtsp_config.enable_audio = config_.enable_rtsp_audio && config_.enable_audio;
        rtsp_server_ = std::make_unique<RTSPServer>(rtsp_config);

        // Wire audio reader to RTSP server for real-time audio streaming
        if (audio_reader_ && rtsp_config.enable_audio) {
            audio_reader_->set_chunk_callback(
                [this](const AudioReader::AudioChunk& chunk) {
                    if (rtsp_server_) {
                        rtsp_server_->push_audio(chunk);
                    }
                });
        }
    }

    // Create virtual camera outputs (v4l2loopback)
    for (const auto& vcam_config : config_.virtual_cameras) {
        if (!vcam_config.enabled) {
            continue;
        }

        auto vcam = std::make_unique<V4L2LoopbackOutput>();
        V4L2LoopbackOutput::Config loopback_config;
        loopback_config.device = vcam_config.device;
        loopback_config.label = vcam_config.label;
        // Use per-vcam dimensions if specified, otherwise use camera output dimensions
        loopback_config.width = (vcam_config.width > 0) ? vcam_config.width : output_width;
        loopback_config.height = (vcam_config.height > 0) ? vcam_config.height : output_height;
        loopback_config.framerate = config_.camera.framerate;

        if (vcam->initialize(loopback_config)) {
            virtual_cameras_.push_back(std::move(vcam));
        } else {
            LOG_WARN("Failed to initialize virtual camera: ", vcam_config.device,
                     " (is v4l2loopback loaded?)");
        }
    }

    if (!virtual_cameras_.empty()) {
        LOG_INFO("Initialized ", virtual_cameras_.size(), " virtual camera output(s)");
    }

    // Create frame rotator for 90°/270° (180° is handled by ISP)
    if (needs_sw_rotation) {
        auto rot = (config_.camera.rotation == 90)
            ? FrameRotator::Rotation::Rot90
            : FrameRotator::Rotation::Rot270;
        frame_rotator_ = std::make_unique<FrameRotator>(
            config_.camera.width, config_.camera.height, rot);
    }

    // Set up callbacks
    camera_manager_->set_frame_callback(
        [this](const FrameMetadata& meta, const uint8_t* data, size_t size) {
            on_raw_frame(meta, data, size);
        }
    );

    encoder_->set_output_callback(
        [this](const EncodedFrame& frame) {
            on_encoded_frame(frame);
        }
    );

    LOG_INFO("Capture pipeline initialized");
    return true;
}

bool CapturePipeline::start() {
    if (running_) {
        return true;
    }

    LOG_INFO("Starting capture pipeline");

    stats_start_time_ = get_timestamp_us();
    last_stats_time_ = stats_start_time_;

    // Start components in order
    if (!still_capture_->start()) {
        LOG_ERROR("Failed to start still capture");
        return false;
    }

    if (!clip_extractor_->start()) {
        LOG_ERROR("Failed to start clip extractor");
        return false;
    }

    // Start audio reader (if configured) - connects SHM synchronously
    if (audio_reader_ && !audio_reader_->start()) {
        LOG_ERROR("Failed to start audio reader — audio SHM not available or not streaming");
        return false;
    }

    // Set audio format on RTSP server before starting it
    if (audio_reader_ && rtsp_server_ && audio_reader_->sample_rate() > 0) {
        rtsp_server_->set_audio_format(
            audio_reader_->sample_rate(),
            audio_reader_->channels(),
            audio_reader_->bits_per_sample());
        LOG_INFO("Set RTSP audio format: {} Hz, {} ch, {} bit",
                 audio_reader_->sample_rate(),
                 audio_reader_->channels(),
                 audio_reader_->bits_per_sample());
    }

    if (rtsp_server_ && !rtsp_server_->start()) {
        LOG_WARN("Failed to start RTSP server (continuing without it)");
    }

    if (!encoder_->start()) {
        LOG_ERROR("Failed to start encoder");
        return false;
    }

    if (!camera_manager_->start()) {
        LOG_ERROR("Failed to start camera");
        return false;
    }

    running_ = true;
    LOG_INFO("Capture pipeline started");
    return true;
}

void CapturePipeline::stop() {
    if (!running_) {
        return;
    }

    LOG_INFO("Stopping capture pipeline");
    running_ = false;

    // Stop in reverse order
    camera_manager_->stop();
    encoder_->stop();
    
    if (rtsp_server_) {
        rtsp_server_->stop();
    }

    // Close virtual camera outputs
    for (auto& vcam : virtual_cameras_) {
        vcam->close();
    }
    virtual_cameras_.clear();
    
    clip_extractor_->stop();
    
    if (audio_reader_) {
        audio_reader_->stop();
    }
    
    still_capture_->stop();

    LOG_INFO("Capture pipeline stopped");
}

std::string CapturePipeline::capture_still(int time_offset_seconds) {
    if (!running_) {
        return "";
    }

    uint64_t request_id = still_capture_->request_capture(time_offset_seconds);
    
    // If time_offset is positive (future), add that to the timeout
    int timeout_ms = 5000;
    if (time_offset_seconds > 0) {
        timeout_ms += time_offset_seconds * 1000;
    }
    return still_capture_->wait_for_capture(request_id, timeout_ms);
}

std::vector<std::string> CapturePipeline::capture_burst(int count, int interval_ms) {
    std::vector<std::string> paths;
    if (!running_ || count <= 0) {
        return paths;
    }

    // Clamp count to reasonable limit
    count = std::min(count, 100);

    // Queue all burst requests
    std::vector<uint64_t> request_ids;
    request_ids.reserve(count);
    for (int i = 0; i < count; i++) {
        std::string prefix = "burst_" + std::to_string(i + 1) + "of" + std::to_string(count) + "_";
        uint64_t id = still_capture_->request_capture(0, prefix);
        request_ids.push_back(id);
        
        // Wait between captures if interval specified
        if (interval_ms > 0 && i < count - 1) {
            std::this_thread::sleep_for(std::chrono::milliseconds(interval_ms));
        }
    }

    // Wait for all to complete
    int timeout_ms = 5000 + count * 1000;  // Extra time for large bursts
    for (uint64_t id : request_ids) {
        std::string path = still_capture_->wait_for_capture(id, timeout_ms);
        if (!path.empty()) {
            paths.push_back(std::move(path));
        }
    }

    LOG_INFO("Burst capture: ", paths.size(), "/", count, " images captured");
    return paths;
}

std::string CapturePipeline::capture_clip(int start_offset, int end_offset) {
    if (!running_) {
        return "";
    }

    uint64_t request_id = clip_extractor_->request_clip(start_offset, end_offset);
    
    // Calculate timeout: if end is in future, wait for it + buffer
    int timeout_ms = 5000;
    if (end_offset > 0) {
        timeout_ms += end_offset * 1000;
    }
    return clip_extractor_->wait_for_clip(request_id, timeout_ms);
}

bool CapturePipeline::set_parameter(const std::string& key, const std::string& value) {
    // Handle tuning file change (warm restart)
    if (key == "tuning_file") {
        return restart_camera(value);
    }

    // Handle special commands
    if (key == "keyframe") {
        encoder_->force_keyframe();
        return true;
    }
    
    // Integer controls
    static const std::set<std::string> int_controls = {
        "exposure", "exposure_time", "ae_metering", "ae_constraint"
    };
    
    // Float controls  
    static const std::set<std::string> float_controls = {
        "gain", "analogue_gain", "brightness", "contrast", 
        "sharpness", "saturation", "exposure_value", "ev"
    };
    
    // Boolean controls
    static const std::set<std::string> bool_controls = {
        "ae_enable", "auto_exposure", "awb_enable", "auto_white_balance"
    };
    
    if (int_controls.count(key)) {
        return camera_manager_->set_control(key, static_cast<int64_t>(std::stoll(value)));
    }
    
    if (float_controls.count(key)) {
        return camera_manager_->set_control(key, std::stof(value));
    }
    
    if (bool_controls.count(key)) {
        // Accept "1", "true", "on" as true
        bool b = (value == "1" || value == "true" || value == "on");
        return camera_manager_->set_control(key, b);
    }
    
    // Try as integer by default
    try {
        return camera_manager_->set_control(key, static_cast<int64_t>(std::stoll(value)));
    } catch (...) {
        LOG_WARN("Unknown parameter: ", key);
        return false;
    }
}

bool CapturePipeline::restart_camera(const std::string& new_tuning_file) {
    LOG_INFO("Warm restart: switching tuning file to ", new_tuning_file);

    // 1. Stop camera capture (guarantees no more frame callbacks)
    camera_manager_->stop();

    // 2. Stop encoder (STREAMOFF drains DMABUFs safely)
    encoder_->stop();

    // 3. Destroy old camera manager (releases camera + libcamera CM)
    camera_manager_.reset();

    // 4. Update config
    config_.camera.tuning_file = new_tuning_file;

    // 5. Rebuild camera manager with new tuning file
    camera_manager_ = std::make_unique<CameraManager>();
    if (!camera_manager_->initialize(config_.camera.tuning_file)) {
        LOG_ERROR("Warm restart failed: camera init");
        return false;
    }
    if (!camera_manager_->configure(config_.camera)) {
        LOG_ERROR("Warm restart failed: camera configure");
        return false;
    }

    // 6. Rebuild encoder (V4L2 buffers are tied to old DMABUFs)
    encoder_.reset();
    encoder_ = std::make_unique<V4L2Encoder>();
    V4L2Encoder::Config enc_config;
    bool needs_sw_rotation = (config_.camera.rotation == 90 || config_.camera.rotation == 270);
    enc_config.width = needs_sw_rotation ? config_.camera.height : config_.camera.width;
    enc_config.height = needs_sw_rotation ? config_.camera.width : config_.camera.height;
    enc_config.framerate = config_.camera.framerate;
    enc_config.bitrate = config_.camera.bitrate;
    enc_config.keyframe_interval = config_.camera.keyframe_interval;
    enc_config.use_userptr = needs_sw_rotation;
    if (!encoder_->initialize(enc_config)) {
        LOG_ERROR("Warm restart failed: encoder init");
        return false;
    }

    // 7. Re-wire callbacks (old lambdas captured 'this' which is still valid)
    camera_manager_->set_frame_callback(
        [this](const FrameMetadata& meta, const uint8_t* data, size_t size) {
            on_raw_frame(meta, data, size);
        }
    );
    encoder_->set_output_callback(
        [this](const EncodedFrame& frame) {
            on_encoded_frame(frame);
        }
    );

    // 8. Start encoder then camera
    if (!encoder_->start()) {
        LOG_ERROR("Warm restart failed: encoder start");
        return false;
    }
    if (!camera_manager_->start()) {
        LOG_ERROR("Warm restart failed: camera start");
        return false;
    }

    // 9. Force immediate keyframe so RTSP clients recover cleanly
    encoder_->force_keyframe();

    LOG_INFO("Warm restart complete with tuning file: ", new_tuning_file);
    return true;
}

std::string CapturePipeline::get_status_json() const {
    auto stats = get_stats();
    auto rb_stats = ring_buffer_->get_stats();
    auto enc_stats = encoder_->get_stats();
    auto still_stats = still_capture_->get_stats();
    auto clip_stats = clip_extractor_->get_stats();

    std::ostringstream json;
    json << std::fixed << std::setprecision(2);
    json << "{";
    json << "\"running\":" << (running_ ? "true" : "false") << ",";
    json << "\"capture\":{";
    json << "\"frames\":" << stats.frames_captured << ",";
    json << "\"fps\":" << stats.capture_fps << ",";
    json << "\"dropped\":" << stats.frames_dropped;
    json << "},";
    json << "\"encoder\":{";
    json << "\"frames_in\":" << enc_stats.frames_in << ",";
    json << "\"frames_out\":" << enc_stats.frames_out << ",";
    json << "\"bytes\":" << enc_stats.bytes_out << ",";
    json << "\"dropped\":" << enc_stats.dropped_frames;
    json << "},";
    json << "\"ring_buffer\":{";
    json << "\"frames\":" << rb_stats.frame_count << ",";
    json << "\"bytes\":" << rb_stats.total_bytes << ",";
    json << "\"duration_ms\":" << rb_stats.duration_ms;
    json << "},";
    json << "\"stills\":{";
    json << "\"captured\":" << still_stats.captures_completed << ",";
    json << "\"failed\":" << still_stats.captures_failed;
    json << "},";
    json << "\"clips\":{";
    json << "\"completed\":" << clip_stats.clips_completed << ",";
    json << "\"failed\":" << clip_stats.clips_failed << ",";
    json << "\"bytes_written\":" << clip_stats.total_bytes_written;
    json << "}";
    
    if (rtsp_server_) {
        auto stream_stats = rtsp_server_->get_stats();
        json << ",\"streaming\":{";
        json << "\"clients\":" << stream_stats.connected_clients << ",";
        json << "\"frames_sent\":" << stream_stats.frames_sent << ",";
        json << "\"bytes_sent\":" << stream_stats.bytes_sent;
        json << "}";
    }

    if (audio_reader_) {
        json << ",\"audio\":{";
        json << "\"connected\":" << (audio_reader_->is_running() ? "true" : "false") << ",";
        json << "\"sample_rate\":" << audio_reader_->sample_rate() << ",";
        json << "\"channels\":" << audio_reader_->channels();
        json << "}";
    }
    
    json << "}";
    return json.str();
}

CapturePipeline::Stats CapturePipeline::get_stats() const {
    Stats stats;
    stats.frames_captured = frames_captured_.load();
    stats.frames_encoded = frames_encoded_.load();
    stats.frames_dropped = frames_dropped_.load();
    stats.capture_fps = capture_fps_;
    stats.encode_fps = encode_fps_;

    auto rb_stats = ring_buffer_->get_stats();
    stats.ring_buffer_frames = rb_stats.frame_count;
    stats.ring_buffer_bytes = rb_stats.total_bytes;

    return stats;
}

void CapturePipeline::on_raw_frame(const FrameMetadata& metadata, const uint8_t* data, size_t size) {
    frames_captured_++;

    // Update FPS calculation periodically
    uint64_t now = get_timestamp_us();
    if (now - last_stats_time_ >= 1000000) {  // Every second
        uint64_t elapsed_us = now - last_stats_time_;
        uint64_t capture_count = frames_captured_.load();
        uint64_t encode_count = frames_encoded_.load();
        
        capture_fps_ = (capture_count - last_capture_count_) * 1000000.0 / elapsed_us;
        encode_fps_ = (encode_count - last_encode_count_) * 1000000.0 / elapsed_us;
        
        last_capture_count_ = capture_count;
        last_encode_count_ = encode_count;
        last_stats_time_ = now;
    }

    // Apply software rotation if needed (90°/270°)
    const uint8_t* frame_data = data;
    size_t frame_size = size;
    FrameMetadata frame_meta = metadata;

    if (frame_rotator_) {
        frame_data = frame_rotator_->rotate(data, metadata.stride);
        frame_size = frame_rotator_->rotated_size();
        frame_meta.width = frame_rotator_->dst_width();
        frame_meta.height = frame_rotator_->dst_height();
        frame_meta.stride = frame_rotator_->dst_stride();
        frame_meta.size = frame_size;
        frame_meta.dmabuf_fd = -1;  // No longer a DMABUF

        // Submit to encoder via USERPTR
        if (!encoder_->encode_frame_userptr(frame_data, frame_size, frame_meta.timestamp_us)) {
            frames_dropped_++;
        }
    } else {
        // Submit to encoder (zero-copy via DMABUF)
        if (!encoder_->encode_frame_dmabuf(metadata.dmabuf_fd, size, metadata.timestamp_us)) {
            frames_dropped_++;
        }
    }

    // Submit to still capture (uses rotated frame if applicable)
    still_capture_->submit_frame(frame_data, frame_size, frame_meta);

    // Write to virtual cameras (v4l2loopback outputs)
    for (auto& vcam : virtual_cameras_) {
        vcam->write_frame(frame_data, frame_size, frame_meta);
    }

    // Publish raw frames to shared memory (optional)
    if (frame_publisher_) {
        frame_publisher_->publish(frame_data, frame_size, frame_meta);
    }

    // Convert and publish BGR frames (optional, for OpenCV consumers)
    if (bgr_frame_publisher_ && !bgr_buffer_.empty()) {
        yuv420_to_bgr(frame_data, bgr_buffer_.data(), 
                      frame_meta.width, frame_meta.height, frame_meta.stride);
        
        FrameMetadata bgr_meta = frame_meta;
        bgr_meta.format = PIXFMT_BGR24;
        bgr_meta.stride = frame_meta.width * 3;
        bgr_meta.size = bgr_buffer_.size();
        
        bgr_frame_publisher_->publish(bgr_buffer_.data(), bgr_buffer_.size(), bgr_meta);
    }
}

void CapturePipeline::on_encoded_frame(const EncodedFrame& frame) {
    frames_encoded_++;

    // Add to ring buffer for clip extraction
    clip_extractor_->add_frame(frame);

    // Push to streaming servers
    if (rtsp_server_) {
        rtsp_server_->push_frame(frame);
    }
}

} // namespace camera_daemon
