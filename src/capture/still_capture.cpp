#include "camera_daemon/still_capture.hpp"
#include "camera_daemon/logger.hpp"
#include <jpeglib.h>
#include <fstream>
#include <filesystem>
#include <chrono>
#include <unordered_map>
#include <cstring>
#include <algorithm>

namespace camera_daemon {

StillCapture::StillCapture(const Config& config) : config_(config) {
    // Ensure output directory exists
    std::filesystem::create_directories(config_.output_dir);
    
    // Try to initialize hardware JPEG encoder
    if (V4L2JpegEncoder::is_available()) {
        hw_jpeg_ = std::make_unique<V4L2JpegEncoder>();
        V4L2JpegEncoder::Config jpeg_config;
        jpeg_config.width = config_.width;
        jpeg_config.height = config_.height;
        jpeg_config.quality = config_.jpeg_quality;
        
        if (hw_jpeg_->initialize(jpeg_config)) {
            LOG_INFO("Hardware JPEG encoder enabled");
        } else {
            LOG_WARN("Hardware JPEG init failed, falling back to libjpeg");
            hw_jpeg_.reset();
        }
    } else {
        LOG_INFO("Hardware JPEG not available, using libjpeg");
    }
}

StillCapture::~StillCapture() {
    stop();
}

bool StillCapture::start() {
    if (running_) {
        return true;
    }

    running_ = true;
    worker_thread_ = std::thread(&StillCapture::worker_thread_func, this);
    
    LOG_INFO("Still capture started, output dir: ", config_.output_dir);
    return true;
}

void StillCapture::stop() {
    if (!running_) {
        return;
    }

    {
        // Must hold request_mutex_ when setting running_ to false
        // to prevent lost-wakeup race with worker's condition_variable::wait()
        std::lock_guard<std::mutex> lock(request_mutex_);
        running_ = false;
    }
    request_cv_.notify_all();

    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
    }
    frame_cv_.notify_all();  // Wake up worker thread waiting for frame

    if (worker_thread_.joinable()) {
        worker_thread_.join();
    }

    LOG_INFO("Still capture stopped");
}

uint64_t StillCapture::request_capture(int time_offset_seconds, const std::string& burst_prefix) {
    uint64_t request_id = next_request_id_++;
    captures_requested_++;

    StillRequest request;
    request.request_id = request_id;
    request.time_offset_seconds = time_offset_seconds;
    request.filename_prefix = burst_prefix.empty() ? "still_" : burst_prefix;

    {
        std::lock_guard<std::mutex> lock(request_mutex_);
        pending_requests_.push(request);
    }
    request_cv_.notify_one();

    LOG_DEBUG("Still capture requested, id=", request_id, ", time_offset=", time_offset_seconds, "s");
    return request_id;
}

void StillCapture::submit_frame(const uint8_t* data, size_t size, const FrameMetadata& metadata) {
    // Copy frame if there's a pending capture request OR capture is in progress
    bool need_copy = capture_in_progress_.load(std::memory_order_acquire);
    if (!need_copy) {
        std::lock_guard<std::mutex> lock(request_mutex_);
        need_copy = !pending_requests_.empty();
    }
    
    // Always update metadata for quick access, but only copy pixel data when needed
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        latest_metadata_ = metadata;
        latest_frame_size_ = size;
        latest_frame_data_ptr_ = data;  // Store pointer for lazy copy
        
        if (need_copy) {
            // Only copy when we need to capture
            latest_frame_.resize(size);
            memcpy(latest_frame_.data(), data, size);
            frame_available_ = true;
            frame_cv_.notify_one();  // Wake up worker waiting for frame
        }
    }
}

std::string StillCapture::wait_for_capture(uint64_t request_id, int timeout_ms) {
    std::unique_lock<std::mutex> lock(result_mutex_);
    
    auto pred = [this, request_id]() {
        return completed_captures_.find(request_id) != completed_captures_.end();
    };

    if (timeout_ms < 0) {
        result_cv_.wait(lock, pred);
    } else {
        if (!result_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred)) {
            return "";  // Timeout
        }
    }

    auto it = completed_captures_.find(request_id);
    if (it != completed_captures_.end()) {
        std::string path = std::move(it->second);
        completed_captures_.erase(it);
        return path;
    }

    return "";
}

StillCapture::Stats StillCapture::get_stats() const {
    uint64_t completed = captures_completed_.load();
    uint64_t total_time = total_encode_time_us_.load();
    
    return {
        captures_requested_.load(),
        completed,
        captures_failed_.load(),
        completed > 0 ? total_time / completed : 0
    };
}

void StillCapture::worker_thread_func() {
    LOG_DEBUG("Still capture worker started");

    while (running_) {
        StillRequest request;
        
        {
            std::unique_lock<std::mutex> lock(request_mutex_);
            request_cv_.wait(lock, [this]() {
                return !running_ || !pending_requests_.empty();
            });

            if (!running_ && pending_requests_.empty()) {
                break;
            }

            if (pending_requests_.empty()) {
                continue;
            }

            // Set flag BEFORE popping so submit_frame knows to copy frames
            capture_in_progress_.store(true, std::memory_order_release);
            request = pending_requests_.front();
            pending_requests_.pop();
        }

        uint64_t request_id = request.request_id;
        int time_offset = request.time_offset_seconds;

        // Handle positive time offset (wait before capturing)
        if (time_offset > 0) {
            LOG_DEBUG("Waiting ", time_offset, "s before capturing still");
            std::this_thread::sleep_for(std::chrono::seconds(time_offset));
        }
        // Negative time offset would require historical frame buffer
        // For now, we capture the current frame for negative offsets too
        // (could be enhanced with raw frame ring buffer in future)
        else if (time_offset < 0) {
            LOG_WARN("Negative time offset (", time_offset, "s) not yet supported for still capture, capturing now");
        }

        // Wait for a frame to be available
        std::vector<uint8_t> frame_copy;
        FrameMetadata metadata;
        
        {
            // Wait for a new frame with condition variable
            std::unique_lock<std::mutex> lock(frame_mutex_);
            if (frame_cv_.wait_for(lock, std::chrono::milliseconds(500), [this]() {
                return frame_available_ || !running_;
            })) {
                if (frame_available_) {
                    frame_copy = std::move(latest_frame_);
                    metadata = latest_metadata_;
                    frame_available_ = false;
                }
            }
        }
        
        capture_in_progress_.store(false, std::memory_order_release);

        if (frame_copy.empty()) {
            LOG_WARN("No frame available for still capture");
            captures_failed_++;
            
            std::lock_guard<std::mutex> lock(result_mutex_);
            completed_captures_[request_id] = "";
            result_cv_.notify_all();
            continue;
        }

        // Encode to JPEG (use hardware if available)
        auto start = std::chrono::steady_clock::now();
        std::string path;
        if (hw_jpeg_) {
            path = encode_jpeg_hardware(frame_copy.data(), frame_copy.size(), metadata, request.filename_prefix);
        } else {
            path = encode_jpeg(frame_copy.data(), metadata, request.filename_prefix);
        }
        auto end = std::chrono::steady_clock::now();

        auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end - start);
        total_encode_time_us_ += duration.count();

        if (path.empty()) {
            LOG_ERROR("JPEG encoding failed");
            captures_failed_++;
        } else {
            LOG_INFO("Still captured: ", path, " (", duration.count() / 1000, " ms)");
            captures_completed_++;
        }

        {
            std::lock_guard<std::mutex> lock(result_mutex_);
            completed_captures_[request_id] = path;
        }
        result_cv_.notify_all();
    }

    LOG_DEBUG("Still capture worker stopped");
}

std::string StillCapture::encode_jpeg(const uint8_t* data, const FrameMetadata& metadata, const std::string& prefix) {
    std::string filename = prefix + timestamp_to_filename() + ".jpg";
    std::string path = config_.output_dir + "/" + filename;

    uint32_t width = metadata.width;
    uint32_t height = metadata.height;
    uint32_t stride = metadata.stride > 0 ? metadata.stride : width;
    uint32_t stride2 = stride / 2;
    
    LOG_DEBUG("YUV420 JPEG encode: ", width, "x", height, " stride=", stride);

    // YUV420 planar format layout:
    // Y plane: stride * height bytes
    // U plane: (stride/2) * (height/2) bytes
    // V plane: (stride/2) * (height/2) bytes
    const uint8_t* Y = data;
    const uint8_t* U = Y + stride * height;
    const uint8_t* V = U + stride2 * (height / 2);
    
    // Calculate max pointers to avoid buffer overrun
    const uint8_t* Y_max = U - stride;
    const uint8_t* U_max = V - stride2;
    const uint8_t* V_max = U_max + stride2 * (height / 2);

    // JPEG compression using native YUV420 support (no RGB conversion needed!)
    struct jpeg_compress_struct cinfo;
    struct jpeg_error_mgr jerr;

    cinfo.err = jpeg_std_error(&jerr);
    jpeg_create_compress(&cinfo);

    FILE* outfile = fopen(path.c_str(), "wb");
    if (!outfile) {
        LOG_ERROR("Failed to open output file: ", path);
        jpeg_destroy_compress(&cinfo);
        return "";
    }

    jpeg_stdio_dest(&cinfo, outfile);

    cinfo.image_width = width;
    cinfo.image_height = height;
    cinfo.input_components = 3;
    cinfo.in_color_space = JCS_YCbCr;
    cinfo.restart_interval = 0;

    jpeg_set_defaults(&cinfo);
    cinfo.raw_data_in = TRUE;  // Direct YUV input, no conversion!
    jpeg_set_quality(&cinfo, config_.jpeg_quality, TRUE);

    jpeg_start_compress(&cinfo, TRUE);

    // Process 16 rows at a time (MCU height for YUV420)
    JSAMPROW y_rows[16];
    JSAMPROW u_rows[8];
    JSAMPROW v_rows[8];

    uint8_t* Y_row = const_cast<uint8_t*>(Y);
    uint8_t* U_row = const_cast<uint8_t*>(U);
    uint8_t* V_row = const_cast<uint8_t*>(V);

    while (cinfo.next_scanline < height) {
        for (int i = 0; i < 16; i++, Y_row += stride)
            y_rows[i] = std::min(Y_row, const_cast<uint8_t*>(Y_max));
        for (int i = 0; i < 8; i++, U_row += stride2, V_row += stride2) {
            u_rows[i] = std::min(U_row, const_cast<uint8_t*>(U_max));
            v_rows[i] = std::min(V_row, const_cast<uint8_t*>(V_max));
        }

        JSAMPARRAY rows[] = { y_rows, u_rows, v_rows };
        jpeg_write_raw_data(&cinfo, rows, 16);
    }

    jpeg_finish_compress(&cinfo);
    fclose(outfile);
    jpeg_destroy_compress(&cinfo);

    return path;
}

std::string StillCapture::encode_jpeg_hardware(const uint8_t* data, size_t size, const FrameMetadata& metadata, const std::string& prefix) {
    std::string filename = prefix + timestamp_to_filename() + ".jpg";
    std::string path = config_.output_dir + "/" + filename;

    std::vector<uint8_t> jpeg_data;
    if (!hw_jpeg_->encode(data, size, jpeg_data)) {
        LOG_WARN("Hardware JPEG failed, falling back to software");
        return encode_jpeg(data, metadata, prefix);  // Fallback
    }

    // Write to file
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        LOG_ERROR("Failed to open output file: ", path);
        return "";
    }
    file.write(reinterpret_cast<const char*>(jpeg_data.data()), jpeg_data.size());
    
    LOG_DEBUG("Hardware JPEG encoded: ", size, " -> ", jpeg_data.size(), " bytes");
    return path;
}

} // namespace camera_daemon
