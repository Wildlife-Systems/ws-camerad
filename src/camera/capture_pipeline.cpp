#include "camera_daemon/capture_pipeline.hpp"
#include "camera_daemon/logger.hpp"
#include <sstream>
#include <iomanip>
#include <algorithm>
#include <set>
#include <linux/dma-buf.h>
#include <sys/ioctl.h>
#include <csetjmp>

extern "C" {
#include <jpeglib.h>
}

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

// ---------------------------------------------------------------------------
// YUYV (4:2:2 packed) → YUV420 (I420 planar) conversion
// Process two rows at a time so we can vertically downsample chroma.
// ---------------------------------------------------------------------------
static void yuyv_to_yuv420(const uint8_t* src, uint8_t* dst,
                           uint32_t width, uint32_t height, uint32_t src_stride) {
    const uint32_t y_stride = width;
    const uint32_t uv_stride = width / 2;
    uint8_t* y_out = dst;
    uint8_t* u_out = y_out + y_stride * height;
    uint8_t* v_out = u_out + uv_stride * (height / 2);

    for (uint32_t row = 0; row < height; row += 2) {
        const uint8_t* row0 = src + row * src_stride;
        const uint8_t* row1 = (row + 1 < height) ? src + (row + 1) * src_stride : row0;
        uint8_t* y0 = y_out + row * y_stride;
        uint8_t* y1 = y_out + (row + 1) * y_stride;
        uint8_t* u  = u_out + (row / 2) * uv_stride;
        uint8_t* v  = v_out + (row / 2) * uv_stride;

        for (uint32_t col = 0; col < width; col += 2) {
            // Row 0: [Y0 U Y1 V]
            uint32_t off0 = col * 2;
            y0[col]     = row0[off0];
            y0[col + 1] = row0[off0 + 2];
            uint8_t u0  = row0[off0 + 1];
            uint8_t v0  = row0[off0 + 3];

            // Row 1: [Y0 U Y1 V]
            uint32_t off1 = col * 2;
            y1[col]     = row1[off1];
            y1[col + 1] = row1[off1 + 2];
            uint8_t u1  = row1[off1 + 1];
            uint8_t v1  = row1[off1 + 3];

            // Average chroma vertically for 4:2:0
            u[col / 2] = static_cast<uint8_t>((u0 + u1 + 1) >> 1);
            v[col / 2] = static_cast<uint8_t>((v0 + v1 + 1) >> 1);
        }
    }
}

// ---------------------------------------------------------------------------
// MJPEG → YUV420 (I420 planar) decode using libjpeg
// Uses jpeg_read_raw_data() to get raw YCbCr without colour-space conversion.
// Handles both 4:2:2 (typical for USB cameras) and 4:2:0 JPEG input.
// ---------------------------------------------------------------------------

// Error manager that longjmps instead of calling exit()
struct JpegErrorMgr {
    struct jpeg_error_mgr pub;
    std::jmp_buf jmpbuf;
};

static void jpeg_error_exit(j_common_ptr cinfo) {
    auto* mgr = reinterpret_cast<JpegErrorMgr*>(cinfo->err);
    char buf[JMSG_LENGTH_MAX];
    cinfo->err->format_message(cinfo, buf);
    LOG_ERROR("MJPEG decode error: ", buf);
    std::longjmp(mgr->jmpbuf, 1);
}

static bool mjpeg_to_yuv420(const uint8_t* jpeg_data, size_t jpeg_size,
                            uint8_t* yuv420, uint32_t /*exp_w*/, uint32_t /*exp_h*/) {
    struct jpeg_decompress_struct cinfo;
    JpegErrorMgr jerr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit = jpeg_error_exit;

    if (setjmp(jerr.jmpbuf)) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, jpeg_data, jpeg_size);

    if (jpeg_read_header(&cinfo, TRUE) != JPEG_HEADER_OK) {
        jpeg_destroy_decompress(&cinfo);
        return false;
    }

    // Request raw YCbCr output — avoids colour-space conversion
    cinfo.raw_data_out = TRUE;
    cinfo.out_color_space = JCS_YCbCr;

    jpeg_start_decompress(&cinfo);

    uint32_t w = cinfo.output_width;
    uint32_t h = cinfo.output_height;
    int max_v = cinfo.max_v_samp_factor;
    int mcu_rows = max_v * DCTSIZE;  // rows per jpeg_read_raw_data() call

    uint8_t* y_out = yuv420;
    uint8_t* u_out = y_out + w * h;
    uint8_t* v_out = u_out + (w / 2) * (h / 2);
    int uv_w = w / 2;

    // Detect 4:2:2 (horizontal-only chroma subsampling)
    bool is_422 = (cinfo.comp_info[0].v_samp_factor == cinfo.comp_info[1].v_samp_factor);
    // In 4:2:0 the Y v_samp_factor is 2× the chroma v_samp_factor

    // Row pointer arrays (max MCU height is 16 for 4:2:0, 8 for 4:2:2)
    JSAMPROW y_rows[DCTSIZE * 2];
    JSAMPROW cb_rows[DCTSIZE];
    JSAMPROW cr_rows[DCTSIZE];
    JSAMPARRAY planes[3] = { y_rows, cb_rows, cr_rows };

    // Temp chroma rows for 4:2:2 → 4:2:0 vertical downsample
    std::vector<uint8_t> cb_tmp, cr_tmp;
    if (is_422) {
        cb_tmp.resize(uv_w * mcu_rows);
        cr_tmp.resize(uv_w * mcu_rows);
    }

    uint32_t y_row = 0;
    while (cinfo.output_scanline < cinfo.output_height) {
        // --- Y row pointers (mcu_rows, up to 16 for 4:2:0) ---
        for (int i = 0; i < mcu_rows; i++) {
            uint32_t r = y_row + i;
            y_rows[i] = (r < h) ? y_out + r * w : y_out + (h - 1) * w;
        }

        // --- chroma row pointers ---
        int chroma_lines = mcu_rows / max_v;  // half for 4:2:0, same for 4:2:2
        if (is_422) {
            for (int i = 0; i < chroma_lines; i++) {
                cb_rows[i] = cb_tmp.data() + i * uv_w;
                cr_rows[i] = cr_tmp.data() + i * uv_w;
            }
        } else {
            uint32_t uv_row = y_row / 2;
            for (int i = 0; i < chroma_lines; i++) {
                uint32_t r = uv_row + i;
                cb_rows[i] = (r < h / 2) ? u_out + r * uv_w : u_out + (h / 2 - 1) * uv_w;
                cr_rows[i] = (r < h / 2) ? v_out + r * uv_w : v_out + (h / 2 - 1) * uv_w;
            }
        }

        jpeg_read_raw_data(&cinfo, planes, mcu_rows);

        // 4:2:2 → 4:2:0: average two chroma rows into one
        if (is_422) {
            uint32_t uv_row = y_row / 2;
            int out_rows = std::min(chroma_lines / 2, static_cast<int>(h / 2 - uv_row));
            for (int i = 0; i < out_rows; i++) {
                const uint8_t* cb0 = cb_tmp.data() + (i * 2) * uv_w;
                const uint8_t* cb1 = cb_tmp.data() + (i * 2 + 1) * uv_w;
                const uint8_t* cr0 = cr_tmp.data() + (i * 2) * uv_w;
                const uint8_t* cr1 = cr_tmp.data() + (i * 2 + 1) * uv_w;
                uint8_t* uo = u_out + (uv_row + i) * uv_w;
                uint8_t* vo = v_out + (uv_row + i) * uv_w;
                for (int j = 0; j < uv_w; j++) {
                    uo[j] = static_cast<uint8_t>((cb0[j] + cb1[j] + 1) >> 1);
                    vo[j] = static_cast<uint8_t>((cr0[j] + cr1[j] + 1) >> 1);
                }
            }
        }

        y_row += mcu_rows;
    }

    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);
    return true;
}

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

    // Read back actual negotiated dimensions and pixel format.
    // The camera may have adjusted resolution/format (e.g. USB cameras that
    // don't support YUV420 or the requested resolution).
    const auto& actual_cam = camera_manager_->config();
    camera_pixel_format_ = camera_manager_->actual_pixel_format();
    config_.camera.width = actual_cam.width;
    config_.camera.height = actual_cam.height;
    LOG_INFO("Actual camera output: ", actual_cam.width, "x", actual_cam.height,
             " format=", static_cast<int>(camera_pixel_format_));

    // Pre-allocate conversion buffer if camera delivers non-YUV420
    if (camera_pixel_format_ != PIXFMT_YUV420) {
        size_t yuv_size = actual_cam.width * actual_cam.height * 3 / 2;
        yuv420_buffer_.resize(yuv_size);
        LOG_INFO("Allocated ", yuv_size, " byte conversion buffer for format ",
                 static_cast<int>(camera_pixel_format_), " → YUV420");
    }

    // Prepare encoder config — actual initialization is deferred to the first
    // frame so we can inspect metadata.dmabuf_fd and choose DMABUF vs copy mode.
    bool needs_sw_rotation = (config_.camera.rotation == 90 || config_.camera.rotation == 270);
    uint32_t output_width = needs_sw_rotation ? actual_cam.height : actual_cam.width;
    uint32_t output_height = needs_sw_rotation ? actual_cam.width : actual_cam.height;

    pending_enc_config_ = {};
    pending_enc_config_.width = output_width;
    pending_enc_config_.height = output_height;
    pending_enc_config_.framerate = config_.camera.framerate;
    pending_enc_config_.bitrate = config_.camera.bitrate;
    pending_enc_config_.keyframe_interval = config_.camera.keyframe_interval;
    // use_userptr will be decided on the first frame (DMABUF detection)
    pending_enc_config_.use_userptr = needs_sw_rotation;
    encoder_initialized_ = false;

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
    clip_extractor_ = std::make_unique<ClipExtractor>(clip_config, *ring_buffer_);

    // Create RTSP server (optional)
    if (config_.enable_rtsp) {
        RTSPServer::Config rtsp_config;
        rtsp_config.port = config_.rtsp_port;
        rtsp_config.width = output_width;
        rtsp_config.height = output_height;
        rtsp_config.framerate = config_.camera.framerate;
        rtsp_server_ = std::make_unique<RTSPServer>(rtsp_config);
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

    // Note: encoder callback and start are deferred to the first frame
    // (on_raw_frame) so we can detect DMABUF support at runtime.

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

    if (rtsp_server_ && !rtsp_server_->start()) {
        LOG_WARN("Failed to start RTSP server (continuing without it)");
    }

    // Encoder starts on the first frame (deferred init for DMABUF detection)

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
    if (encoder_) encoder_->stop();
    
    if (rtsp_server_) {
        rtsp_server_->stop();
    }

    // Close virtual camera outputs
    for (auto& vcam : virtual_cameras_) {
        vcam->close();
    }
    virtual_cameras_.clear();
    
    clip_extractor_->stop();
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
        if (encoder_) encoder_->force_keyframe();
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
    if (encoder_) encoder_->stop();

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

    // 6. Rebuild encoder — defer to first frame for DMABUF re-detection
    encoder_.reset();
    bool needs_sw_rotation = (config_.camera.rotation == 90 || config_.camera.rotation == 270);
    pending_enc_config_ = {};
    pending_enc_config_.width = needs_sw_rotation ? config_.camera.height : config_.camera.width;
    pending_enc_config_.height = needs_sw_rotation ? config_.camera.width : config_.camera.height;
    pending_enc_config_.framerate = config_.camera.framerate;
    pending_enc_config_.bitrate = config_.camera.bitrate;
    pending_enc_config_.keyframe_interval = config_.camera.keyframe_interval;
    pending_enc_config_.use_userptr = needs_sw_rotation;
    encoder_initialized_ = false;

    // 7. Re-wire camera callback (encoder callback is set on first frame)
    camera_manager_->set_frame_callback(
        [this](const FrameMetadata& meta, const uint8_t* data, size_t size) {
            on_raw_frame(meta, data, size);
        }
    );

    // 8. Start camera (encoder starts on first frame)
    if (!camera_manager_->start()) {
        LOG_ERROR("Warm restart failed: camera start");
        return false;
    }

    LOG_INFO("Warm restart complete with tuning file: ", new_tuning_file);
    return true;
}

std::string CapturePipeline::get_status_json() const {
    auto stats = get_stats();
    auto rb_stats = ring_buffer_->get_stats();
    auto enc_stats = encoder_ ? encoder_->get_stats() : V4L2Encoder::Stats{};
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

    // ---- Format conversion: ensure downstream always sees YUV420 planar ----
    const uint8_t* yuv_data = data;
    size_t yuv_size = size;
    FrameMetadata yuv_meta = metadata;

    if (metadata.format == PIXFMT_YUYV) {
        size_t needed = metadata.width * metadata.height * 3 / 2;
        if (yuv420_buffer_.size() < needed) yuv420_buffer_.resize(needed);

        yuyv_to_yuv420(data, yuv420_buffer_.data(),
                       metadata.width, metadata.height, metadata.stride);

        yuv_data = yuv420_buffer_.data();
        yuv_size = needed;
        yuv_meta.format = PIXFMT_YUV420;
        yuv_meta.stride = metadata.width;          // packed Y plane, no padding
        yuv_meta.size = needed;
        yuv_meta.dmabuf_fd = -1;                   // conversion buffer, not DMA
        LOG_DEBUG("Converted YUYV→YUV420 ", metadata.width, "x", metadata.height);
    } else if (metadata.format == PIXFMT_MJPEG) {
        size_t needed = metadata.width * metadata.height * 3 / 2;
        if (yuv420_buffer_.size() < needed) yuv420_buffer_.resize(needed);

        if (!mjpeg_to_yuv420(data, size,
                             yuv420_buffer_.data(), metadata.width, metadata.height)) {
            LOG_ERROR("MJPEG decode failed, dropping frame");
            frames_dropped_++;
            return;
        }

        yuv_data = yuv420_buffer_.data();
        yuv_size = needed;
        yuv_meta.format = PIXFMT_YUV420;
        yuv_meta.stride = metadata.width;
        yuv_meta.size = needed;
        yuv_meta.dmabuf_fd = -1;
        LOG_DEBUG("Decoded MJPEG→YUV420 ", metadata.width, "x", metadata.height);
    }
    // else: already YUV420, use data/size/metadata as-is

    // Deferred encoder init: on the first frame, optimistically try DMABUF
    // mode (zero-copy).  If it fails (USB/UVC cameras whose DMA-BUFs live in
    // system RAM and can't be imported by bcm2835-codec), tear down the
    // encoder and rebuild in copy mode.  This is the only reliable detection
    // because UVC DMA-BUFs pass DMA_BUF_IOCTL_SYNC yet still fail VIDIOC_QBUF.
    if (!encoder_initialized_) {
        bool try_dmabuf = !frame_rotator_ && yuv_meta.dmabuf_fd >= 0;

        pending_enc_config_.use_userptr = !try_dmabuf;
        LOG_INFO("Encoder init: trying ", try_dmabuf ? "DMABUF zero-copy" : "copy", " mode");

        encoder_ = std::make_unique<V4L2Encoder>();
        if (!encoder_->initialize(pending_enc_config_)) {
            LOG_ERROR("Failed to initialize encoder on first frame");
            return;
        }
        encoder_->set_output_callback(
            [this](const EncodedFrame& frame) { on_encoded_frame(frame); });
        if (!encoder_->start()) {
            LOG_ERROR("Failed to start encoder");
            return;
        }

        // Probe: try to queue this frame as DMABUF
        if (try_dmabuf) {
            if (!encoder_->encode_frame_dmabuf(yuv_meta.dmabuf_fd, yuv_size, yuv_meta.timestamp_us)) {
                // DMABUF rejected — rebuild encoder in copy mode
                LOG_WARN("DMABUF queue failed, falling back to copy mode (USB/UVC camera)");
                encoder_->stop();
                encoder_.reset();

                pending_enc_config_.use_userptr = true;
                encoder_ = std::make_unique<V4L2Encoder>();
                if (!encoder_->initialize(pending_enc_config_)) {
                    LOG_ERROR("Failed to reinitialize encoder in copy mode");
                    return;
                }
                encoder_->set_output_callback(
                    [this](const EncodedFrame& frame) { on_encoded_frame(frame); });
                if (!encoder_->start()) {
                    LOG_ERROR("Failed to start encoder in copy mode");
                    return;
                }
                // Submit this frame via copy instead
                encoder_->encode_frame_userptr(yuv_data, yuv_size, yuv_meta.timestamp_us);
            }
        }

        encoder_initialized_ = true;
    }

    // Apply software rotation if needed (90°/270°)
    const uint8_t* frame_data = yuv_data;
    size_t frame_size = yuv_size;
    FrameMetadata frame_meta = yuv_meta;

    if (frame_rotator_) {
        frame_data = frame_rotator_->rotate(yuv_data, yuv_meta.stride);
        frame_size = frame_rotator_->rotated_size();
        frame_meta.width = frame_rotator_->dst_width();
        frame_meta.height = frame_rotator_->dst_height();
        frame_meta.stride = frame_rotator_->dst_stride();
        frame_meta.size = frame_size;
        frame_meta.dmabuf_fd = -1;  // No longer a DMABUF

        // Submit to encoder via copy (MMAP buffers)
        if (!encoder_->encode_frame_userptr(frame_data, frame_size, frame_meta.timestamp_us)) {
            frames_dropped_++;
        }
    } else if (!encoder_->is_userptr_mode() && yuv_meta.dmabuf_fd >= 0) {
        // Submit to encoder (zero-copy via DMABUF)
        if (!encoder_->encode_frame_dmabuf(yuv_meta.dmabuf_fd, yuv_size, yuv_meta.timestamp_us)) {
            frames_dropped_++;
        }
    } else {
        // USB camera or other non-DMABUF source — copy into encoder
        if (!encoder_->encode_frame_userptr(yuv_data, yuv_size, yuv_meta.timestamp_us)) {
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
