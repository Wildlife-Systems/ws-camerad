#include "camera_daemon/clip_extractor.hpp"
#include "camera_daemon/logger.hpp"
#include <fstream>
#include <filesystem>

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#endif

namespace camera_daemon {

ClipExtractor::ClipExtractor(const Config& config, EncodedRingBuffer& ring_buffer)
    : config_(config), ring_buffer_(ring_buffer) {
    // Ensure output directory exists
    std::filesystem::create_directories(config_.output_dir);
}

ClipExtractor::~ClipExtractor() {
    stop();
}

bool ClipExtractor::start() {
    if (running_) {
        return true;
    }

    running_ = true;
    dispatcher_thread_ = std::thread(&ClipExtractor::dispatcher_thread_func, this);

    LOG_INFO("Clip extractor started, output dir: ", config_.output_dir,
             ", max_concurrent: ", config_.max_concurrent);
    return true;
}

void ClipExtractor::stop() {
    if (!running_) {
        return;
    }

    {
        // Must hold request_mutex_ when setting running_ to false
        // to prevent lost-wakeup race with dispatcher's condition_variable::wait()
        std::lock_guard<std::mutex> lock(request_mutex_);
        running_ = false;
    }
    request_cv_.notify_all();

    // Wake up all active recordings
    {
        std::lock_guard<std::mutex> lock(recordings_mutex_);
        for (auto& rec : active_recordings_) {
            std::lock_guard<std::mutex> rec_lock(rec->mutex);
            rec->done = true;
            rec->cv.notify_all();
        }
    }

    if (dispatcher_thread_.joinable()) {
        dispatcher_thread_.join();
    }

    // Join all worker threads
    {
        std::lock_guard<std::mutex> lock(workers_mutex_);
        for (auto& t : clip_workers_) {
            if (t.joinable()) {
                t.join();
            }
        }
        clip_workers_.clear();
    }

    LOG_INFO("Clip extractor stopped");
}

uint64_t ClipExtractor::request_clip(int start_offset, int end_offset) {
    uint64_t request_id = next_request_id_++;
    clips_requested_++;

    ClipRequest request;
    request.request_id = request_id;
    request.start_offset = start_offset;
    request.end_offset = end_offset;
    request.request_timestamp = get_timestamp_us();

    {
        std::lock_guard<std::mutex> lock(request_mutex_);
        pending_requests_.push(request);
    }
    request_cv_.notify_one();

    int duration = end_offset - start_offset;
    LOG_DEBUG("Clip requested: id=", request_id, ", from ", start_offset, 
              "s to ", end_offset, "s (", duration, "s)");
    return request_id;
}

void ClipExtractor::add_frame(EncodedFrame frame) {
    // Always push to ring buffer first
    {
        std::lock_guard<std::mutex> lock(recordings_mutex_);

        if (active_recordings_.empty()) {
            // No active recordings - move directly to ring buffer
            ring_buffer_.push(std::move(frame));
            return;
        }

        // Feed frame to all active post-event recordings
        bool any_active = false;
        for (auto& rec : active_recordings_) {
            std::lock_guard<std::mutex> rec_lock(rec->mutex);
            if (!rec->done) {
                if (frame.metadata.timestamp_us <= rec->end_timestamp) {
                    rec->frames.push_back(frame);  // Copy to each recording
                    any_active = true;
                } else {
                    // This recording's post-event window has ended
                    rec->done = true;
                    rec->cv.notify_all();
                }
            }
        }

        // Push to ring buffer (move if no active recordings needed the copy)
        if (any_active) {
            ring_buffer_.push(frame);  // Copy since recordings took copies
        } else {
            ring_buffer_.push(std::move(frame));
        }
    }
}

std::string ClipExtractor::wait_for_clip(uint64_t request_id, int timeout_ms) {
    std::unique_lock<std::mutex> lock(result_mutex_);

    auto pred = [this, request_id]() {
        return completed_clips_.find(request_id) != completed_clips_.end();
    };

    if (timeout_ms < 0) {
        result_cv_.wait(lock, pred);
    } else {
        if (!result_cv_.wait_for(lock, std::chrono::milliseconds(timeout_ms), pred)) {
            return "";  // Timeout
        }
    }

    auto it = completed_clips_.find(request_id);
    if (it != completed_clips_.end()) {
        std::string path = std::move(it->second);
        completed_clips_.erase(it);
        return path;
    }

    return "";
}

ClipExtractor::Stats ClipExtractor::get_stats() const {
    return {
        clips_requested_.load(),
        clips_completed_.load(),
        clips_failed_.load(),
        total_bytes_written_.load()
    };
}

void ClipExtractor::dispatcher_thread_func() {
    LOG_DEBUG("Clip extractor dispatcher started");

    while (running_) {
        ClipRequest request;

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

            request = pending_requests_.front();
            pending_requests_.pop();
        }

        // Wait if we're at max concurrent extractions
        while (active_extractions_.load() >= config_.max_concurrent && running_) {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }

        if (!running_) break;

        // Spawn a worker thread for this clip
        active_extractions_++;
        std::lock_guard<std::mutex> lock(workers_mutex_);

        // Clean up finished worker threads
        clip_workers_.erase(
            std::remove_if(clip_workers_.begin(), clip_workers_.end(),
                [](std::thread& t) {
                    if (t.joinable()) {
                        // Check if thread is done by trying to join with zero timeout
                        // Can't do that portably, so just join completed ones
                        return false;
                    }
                    return true;
                }),
            clip_workers_.end()
        );

        clip_workers_.emplace_back(&ClipExtractor::clip_worker_func, this, request);
    }

    LOG_DEBUG("Clip extractor dispatcher stopped");
}

void ClipExtractor::clip_worker_func(ClipRequest request) {
    LOG_DEBUG("Clip worker started for request ", request.request_id);

    std::string path = extract_clip(request);

    if (path.empty()) {
        LOG_ERROR("Clip extraction failed for request ", request.request_id);
        clips_failed_++;
    } else {
        LOG_INFO("Clip extracted: ", path);
        clips_completed_++;
    }

    {
        std::lock_guard<std::mutex> lock(result_mutex_);
        completed_clips_[request.request_id] = path;
    }
    result_cv_.notify_all();

    active_extractions_--;
    LOG_DEBUG("Clip worker finished for request ", request.request_id);
}

std::string ClipExtractor::extract_clip(const ClipRequest& request) {
    // start_offset: start time relative to request (negative=past, positive=future)
    // end_offset: end time relative to request (negative=past, positive=future)
    // e.g., -5 to 5 = capture from 5s ago to 5s from now (10s total)
    
    int start_offset = request.start_offset;
    int end_offset = request.end_offset;
    int duration = end_offset - start_offset;
    
    if (duration <= 0) {
        LOG_ERROR("Invalid clip range: start=", start_offset, " end=", end_offset);
        return "";
    }
    
    // If start is in the future, wait for it
    if (start_offset > 0) {
        LOG_DEBUG("Waiting ", start_offset, "s for clip start");
        std::this_thread::sleep_for(std::chrono::seconds(start_offset));
        // Adjust offsets since we've waited
        end_offset -= start_offset;
        start_offset = 0;
    }
    
    // Now start_offset <= 0, end_offset could be positive (future) or negative (all in past)
    int pre_seconds = -start_offset;  // How far back to go (positive value)
    int post_seconds = std::max(0, end_offset);  // How long to record going forward
    
    // Clamp pre_seconds to ring buffer capacity
    uint32_t max_pre = static_cast<uint32_t>(ring_buffer_.max_duration_seconds());
    if (static_cast<uint32_t>(pre_seconds) > max_pre) {
        LOG_WARN("Requested ", pre_seconds, "s pre-event but ring buffer capacity is ",
                 max_pre, "s — clamping to ", max_pre, "s");
        pre_seconds = max_pre;
    }
    
    LOG_DEBUG("Extracting clip: pre=", pre_seconds, "s, post=", post_seconds, "s");
    
    // Extract pre-event frames from ring buffer
    auto pre_frames = ring_buffer_.extract_last_seconds(pre_seconds);
    
    if (pre_frames.empty()) {
        LOG_WARN("No pre-event frames available");
    } else {
        // Warn if buffer hasn't filled to the requested duration yet
        uint64_t actual_duration_ms = 
            (pre_frames.back().metadata.timestamp_us - pre_frames.front().metadata.timestamp_us) / 1000;
        uint64_t requested_ms = static_cast<uint64_t>(pre_seconds) * 1000;
        if (actual_duration_ms < requested_ms * 80 / 100) {  // <80% of requested
            LOG_WARN("Requested ", pre_seconds, "s pre-event but buffer only contains ",
                     actual_duration_ms / 1000, ".", (actual_duration_ms % 1000) / 100,
                     "s of data (buffer still filling)");
        }
    }

    // Set up post-event recording
    std::vector<EncodedFrame> post_frames;
    
    if (post_seconds > 0) {
        // Create an active recording entry
        auto recording = std::make_shared<ActiveRecording>();
        recording->request_id = request.request_id;
        recording->end_timestamp = get_timestamp_us() + (post_seconds * 1000000ULL);

        {
            std::lock_guard<std::mutex> lock(recordings_mutex_);
            active_recordings_.push_back(recording);
        }

        // Wait for post-event recording to complete
        {
            std::unique_lock<std::mutex> lock(recording->mutex);
            recording->cv.wait_for(lock,
                std::chrono::seconds(post_seconds + 2),
                [&recording, this]() {
                    return !running_ || recording->done;
                });

            post_frames = std::move(recording->frames);
            recording->done = true;
        }

        // Remove from active recordings
        {
            std::lock_guard<std::mutex> lock(recordings_mutex_);
            active_recordings_.remove(recording);
        }
    }

    // Combine pre and post frames
    std::vector<EncodedFrame> all_frames;
    all_frames.reserve(pre_frames.size() + post_frames.size());
    
    for (auto& f : pre_frames) {
        all_frames.push_back(std::move(f));
    }
    for (auto& f : post_frames) {
        all_frames.push_back(std::move(f));
    }

    if (all_frames.empty()) {
        LOG_ERROR("No frames to write for clip");
        return "";
    }

    std::string base_name = "clip_" + timestamp_to_filename() + "_" + std::to_string(request.request_id);
    std::string base_path = config_.output_dir + "/" + base_name;

    // Try to write directly to MP4 with timestamps
    if (config_.remux_to_mp4) {
        std::string mp4_path = write_mp4(all_frames, base_path);
        if (!mp4_path.empty()) {
            return mp4_path;
        }
        LOG_WARN("Direct MP4 write failed, falling back to raw H.264");
    }

    // Fall back to raw H.264
    return write_raw_h264(all_frames, base_path);
}

std::string ClipExtractor::write_raw_h264(const std::vector<EncodedFrame>& frames, const std::string& base_path) {
    std::string path = base_path + ".h264";
    
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        LOG_ERROR("Failed to open output file: ", path);
        return "";
    }

    size_t total_bytes = 0;
    for (const auto& frame : frames) {
        file.write(reinterpret_cast<const char*>(frame.data.data()), frame.data.size());
        total_bytes += frame.data.size();
    }

    file.close();
    total_bytes_written_ += total_bytes;

    LOG_DEBUG("Wrote ", frames.size(), " frames, ", total_bytes, " bytes to ", path);
    return path;
}

std::string ClipExtractor::write_mp4(const std::vector<EncodedFrame>& frames, const std::string& base_path) {
#ifdef HAVE_GSTREAMER
    std::string mp4_path = base_path + ".mp4";

    // Create pipeline: appsrc ! h264parse ! mp4mux ! filesink
    GstElement* pipeline = gst_pipeline_new("mp4-writer");
    GstElement* appsrc = gst_element_factory_make("appsrc", "source");
    GstElement* h264parse = gst_element_factory_make("h264parse", "parser");
    GstElement* mp4mux = gst_element_factory_make("mp4mux", "muxer");
    GstElement* filesink = gst_element_factory_make("filesink", "sink");

    if (!pipeline || !appsrc || !h264parse || !mp4mux || !filesink) {
        LOG_WARN("Failed to create GStreamer elements for MP4 writing");
        if (pipeline) gst_object_unref(pipeline);
        if (appsrc) gst_object_unref(appsrc);
        if (h264parse) gst_object_unref(h264parse);
        if (mp4mux) gst_object_unref(mp4mux);
        if (filesink) gst_object_unref(filesink);
        return "";
    }

    // Configure elements
    g_object_set(appsrc,
        "stream-type", 0,  // GST_APP_STREAM_TYPE_STREAM
        "format", GST_FORMAT_TIME,
        "is-live", FALSE,
        nullptr);
    
    // Set caps for H.264
    GstCaps* caps = gst_caps_new_simple("video/x-h264",
        "stream-format", G_TYPE_STRING, "byte-stream",
        "alignment", G_TYPE_STRING, "au",
        "framerate", GST_TYPE_FRACTION, 30, 1,
        nullptr);
    gst_app_src_set_caps(GST_APP_SRC(appsrc), caps);
    gst_caps_unref(caps);

    g_object_set(filesink, "location", mp4_path.c_str(), nullptr);

    // Build pipeline
    gst_bin_add_many(GST_BIN(pipeline), appsrc, h264parse, mp4mux, filesink, nullptr);
    if (!gst_element_link_many(appsrc, h264parse, mp4mux, filesink, nullptr)) {
        LOG_WARN("Failed to link GStreamer pipeline for MP4 writing");
        gst_object_unref(pipeline);
        return "";
    }

    // Start pipeline
    gst_element_set_state(pipeline, GST_STATE_PLAYING);

    // Push frames with timestamps
    uint64_t base_ts = frames.empty() ? 0 : frames[0].metadata.timestamp_us;
    size_t total_bytes = 0;
    
    for (const auto& frame : frames) {
        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, frame.data.size(), nullptr);
        gst_buffer_fill(buffer, 0, frame.data.data(), frame.data.size());
        
        // Set timestamps (convert from microseconds to nanoseconds)
        uint64_t pts_ns = (frame.metadata.timestamp_us - base_ts) * 1000;
        GST_BUFFER_PTS(buffer) = pts_ns;
        GST_BUFFER_DTS(buffer) = pts_ns;
        GST_BUFFER_DURATION(buffer) = GST_SECOND / 30;  // 30fps
        
        if (frame.metadata.is_keyframe) {
            GST_BUFFER_FLAG_UNSET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
        } else {
            GST_BUFFER_FLAG_SET(buffer, GST_BUFFER_FLAG_DELTA_UNIT);
        }

        GstFlowReturn ret = gst_app_src_push_buffer(GST_APP_SRC(appsrc), buffer);
        if (ret != GST_FLOW_OK) {
            LOG_WARN("GStreamer buffer push failed: ", ret);
            break;
        }
        total_bytes += frame.data.size();
    }

    // Signal end of stream
    gst_app_src_end_of_stream(GST_APP_SRC(appsrc));

    // Wait for EOS or error
    GstBus* bus = gst_element_get_bus(pipeline);
    GstMessage* msg = gst_bus_timed_pop_filtered(bus, 30 * GST_SECOND,
        static_cast<GstMessageType>(GST_MESSAGE_ERROR | GST_MESSAGE_EOS));

    bool success = true;
    if (msg) {
        if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
            GError* err = nullptr;
            gst_message_parse_error(msg, &err, nullptr);
            LOG_WARN("GStreamer MP4 write error: ", err->message);
            g_error_free(err);
            success = false;
        }
        gst_message_unref(msg);
    } else {
        LOG_WARN("GStreamer MP4 write timed out");
        success = false;
    }

    gst_object_unref(bus);
    gst_element_set_state(pipeline, GST_STATE_NULL);
    gst_object_unref(pipeline);

    if (success) {
        total_bytes_written_ += total_bytes;
        LOG_DEBUG("Wrote ", frames.size(), " frames to ", mp4_path);
        return mp4_path;
    }
    
    // Clean up failed file
    std::filesystem::remove(mp4_path);
    return "";
#else
    LOG_WARN("GStreamer not available, cannot write MP4");
    return "";
#endif
}

} // namespace camera_daemon
