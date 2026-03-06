#include "camera_daemon/rtsp_server.hpp"
#include "camera_daemon/logger.hpp"
#include <cstring>
#include <memory>

#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/app/gstappsrc.h>

namespace camera_daemon {

// GStreamer data structure - defined in anonymous namespace since it's implementation detail
namespace {
struct GstDataImpl {
    GstRTSPServer* server = nullptr;
    GstRTSPMountPoints* mounts = nullptr;
    GstRTSPMediaFactory* factory = nullptr;
    GMainLoop* loop = nullptr;
    GMainContext* context = nullptr;
    RTSPServer* owner = nullptr;
    
    // Active appsrc elements (one per connected client's media)
    std::mutex appsrc_mutex;
    std::vector<GstAppSrc*> appsrcs;
    
    // Audio appsrc elements (parallel to appsrcs)
    std::vector<GstAppSrc*> audio_appsrcs;
    bool audio_enabled = false;
    uint32_t audio_sample_rate = 0;
    uint16_t audio_channels = 0;
    uint16_t audio_bits = 0;
    
    // Cached keyframe for immediate playback when clients connect
    std::mutex keyframe_mutex;
    std::shared_ptr<const std::vector<uint8_t>> last_keyframe;
    uint64_t keyframe_timestamp_us = 0;
    
    // Stats
    std::atomic<uint64_t>* frames_sent = nullptr;
    std::atomic<uint64_t>* bytes_sent = nullptr;
    std::atomic<size_t>* client_count = nullptr;
};

// Callback when media is unprepared - remove the appsrc
void media_unprepared_cb(GstRTSPMedia* media, gpointer user_data) {
    GstDataImpl* gst_data = static_cast<GstDataImpl*>(user_data);
    
    std::lock_guard<std::mutex> lock(gst_data->appsrc_mutex);
    gst_data->appsrcs.clear();  // Clear since shared media is done
    gst_data->audio_appsrcs.clear();
    LOG_INFO("RTSP media unprepared, appsrc cleared");
}

// Helper to push a buffer to appsrc
// Helper to push a buffer to appsrc - for live streaming, we let GStreamer handle timestamps
static void push_buffer_to_appsrc(GstAppSrc* appsrc, const uint8_t* data, size_t size) {
    GstBuffer* buffer = gst_buffer_new_allocate(nullptr, size, nullptr);
    
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
        memcpy(map.data, data, size);
        gst_buffer_unmap(buffer, &map);
    }
    
    // Let do-timestamp handle timing for live streams
    GST_BUFFER_PTS(buffer) = GST_CLOCK_TIME_NONE;
    GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
    
    gst_app_src_push_buffer(appsrc, buffer);
}

// Callback when media is configured - get the appsrc element
void media_configure_cb(GstRTSPMediaFactory* factory, GstRTSPMedia* media, gpointer user_data) {
    GstDataImpl* gst_data = static_cast<GstDataImpl*>(user_data);
    
    GstElement* element = gst_rtsp_media_get_element(media);
    GstElement* appsrc = gst_bin_get_by_name_recurse_up(GST_BIN(element), "source");
    
    if (appsrc) {
        // Configure appsrc for live streaming
        g_object_set(appsrc,
            "stream-type", GST_APP_STREAM_TYPE_STREAM,
            "format", GST_FORMAT_TIME,
            "is-live", TRUE,
            "do-timestamp", TRUE,
            "max-bytes", G_GUINT64_CONSTANT(0),  // No limit
            "block", FALSE,
            nullptr);
        
        // Set caps for H.264 byte-stream
        GstCaps* caps = gst_caps_new_simple("video/x-h264",
            "stream-format", G_TYPE_STRING, "byte-stream",
            "alignment", G_TYPE_STRING, "au",
            nullptr);
        gst_app_src_set_caps(GST_APP_SRC(appsrc), caps);
        gst_caps_unref(caps);
        
        // Push cached keyframe immediately for instant playback
        {
            std::lock_guard<std::mutex> kf_lock(gst_data->keyframe_mutex);
            if (gst_data->last_keyframe && !gst_data->last_keyframe->empty()) {
                push_buffer_to_appsrc(GST_APP_SRC(appsrc), 
                                      gst_data->last_keyframe->data(),
                                      gst_data->last_keyframe->size());
                LOG_INFO("Pushed cached keyframe (", gst_data->last_keyframe->size(), " bytes) to new client");
            }
        }
        
        {
            std::lock_guard<std::mutex> lock(gst_data->appsrc_mutex);
            // For shared media, only add once
            if (std::find(gst_data->appsrcs.begin(), gst_data->appsrcs.end(), 
                          GST_APP_SRC(appsrc)) == gst_data->appsrcs.end()) {
                gst_data->appsrcs.push_back(GST_APP_SRC(appsrc));
            }
        }
        
        // Connect to media's unprepared signal
        g_signal_connect(media, "unprepared", G_CALLBACK(media_unprepared_cb), gst_data);
        
        LOG_INFO("RTSP client connected, appsrc configured");
        gst_object_unref(appsrc);
    }
    
    // Configure audio appsrc if audio is enabled
    if (gst_data->audio_enabled) {
        GstElement* audio_src = gst_bin_get_by_name_recurse_up(GST_BIN(element), "audiosource");
        if (audio_src) {
            g_object_set(audio_src,
                "stream-type", GST_APP_STREAM_TYPE_STREAM,
                "format", GST_FORMAT_TIME,
                "is-live", TRUE,
                "do-timestamp", TRUE,
                "max-bytes", G_GUINT64_CONSTANT(0),
                "block", FALSE,
                nullptr);
            
            // Set caps for raw PCM audio (matching ws-audiod bit depth)
            const char* audio_fmt = (gst_data->audio_bits == 32) ? "S32LE" : "S16LE";
            GstCaps* audio_caps = gst_caps_new_simple("audio/x-raw",
                "format", G_TYPE_STRING, audio_fmt,
                "rate", G_TYPE_INT, (int)gst_data->audio_sample_rate,
                "channels", G_TYPE_INT, (int)gst_data->audio_channels,
                "layout", G_TYPE_STRING, "interleaved",
                nullptr);
            gst_app_src_set_caps(GST_APP_SRC(audio_src), audio_caps);
            gst_caps_unref(audio_caps);
            
            {
                std::lock_guard<std::mutex> lock(gst_data->appsrc_mutex);
                if (std::find(gst_data->audio_appsrcs.begin(), gst_data->audio_appsrcs.end(),
                              GST_APP_SRC(audio_src)) == gst_data->audio_appsrcs.end()) {
                    gst_data->audio_appsrcs.push_back(GST_APP_SRC(audio_src));
                }
            }
            
            // Push a silent buffer so GStreamer can preroll the audio appsrc
            // (analogous to pushing the cached keyframe for video)
            size_t silence_bytes = static_cast<size_t>(gst_data->audio_sample_rate)
                * gst_data->audio_channels * (gst_data->audio_bits / 8) / 50; // 20ms
            GstBuffer* silence = gst_buffer_new_allocate(nullptr, silence_bytes, nullptr);
            gst_buffer_memset(silence, 0, 0, silence_bytes);
            GST_BUFFER_PTS(silence) = GST_CLOCK_TIME_NONE;
            GST_BUFFER_DTS(silence) = GST_CLOCK_TIME_NONE;
            gst_app_src_push_buffer(GST_APP_SRC(audio_src), silence);

            LOG_INFO("RTSP audio appsrc configured: ", gst_data->audio_sample_rate,
                     " Hz, ", gst_data->audio_channels, " ch");
            gst_object_unref(audio_src);
        }
    }
    
    gst_object_unref(element);
}
} // anonymous namespace

// Wrapper struct that wraps the impl
struct RTSPServer::GstData : public GstDataImpl {};

RTSPServer::RTSPServer(const Config& config) : config_(config) {
    gst_data_ = std::make_unique<GstData>();
    gst_data_->owner = this;
    gst_data_->audio_enabled = config.enable_audio;
}

RTSPServer::~RTSPServer() {
    stop();
}

bool RTSPServer::start() {
    if (running_) {
        return true;
    }

    gst_init(nullptr, nullptr);
    
    gst_data_->server = gst_rtsp_server_new();
    gst_rtsp_server_set_service(gst_data_->server, std::to_string(config_.port).c_str());
    
    gst_data_->mounts = gst_rtsp_server_get_mount_points(gst_data_->server);
    gst_data_->factory = gst_rtsp_media_factory_new();
    
    // Create launch string with appsrc
    // Minimal queue for low-latency live streaming
    std::string launch;
    if (config_.enable_audio && gst_data_->audio_sample_rate > 0) {
        // Video + audio pipeline
        // Audio caps must be in the launch string so GStreamer can construct
        // the SDP during DESCRIBE without waiting for appsrc data
        std::string audio_format = (gst_data_->audio_bits == 32) ? "S32LE" : "S16LE";
        std::string audio_caps = "audio/x-raw,format=" + audio_format + ",rate="
            + std::to_string(gst_data_->audio_sample_rate)
            + ",channels=" + std::to_string(gst_data_->audio_channels)
            + ",layout=interleaved";
        launch = "( appsrc name=source is-live=true format=time do-timestamp=true min-latency=0 ! "
                 "queue max-size-buffers=1 max-size-time=0 max-size-bytes=0 leaky=downstream ! "
                 "h264parse ! rtph264pay name=pay0 pt=96 config-interval=1 "
                 "appsrc name=audiosource is-live=true format=time do-timestamp=true min-latency=0 "
                 "caps=\"" + audio_caps + "\" ! "
                 "queue max-size-buffers=10 max-size-time=500000000 max-size-bytes=0 leaky=downstream ! "
                 "audioconvert ! audioresample ! rtpL16pay name=pay1 pt=97 )";
    } else {
        // Video only pipeline
        launch = "( appsrc name=source is-live=true format=time ! "
                 "queue max-size-buffers=1 max-size-time=0 max-size-bytes=0 leaky=downstream ! "
                 "h264parse ! rtph264pay name=pay0 pt=96 config-interval=1 )";
    }
    
    gst_rtsp_media_factory_set_launch(gst_data_->factory, launch.c_str());
    gst_rtsp_media_factory_set_shared(gst_data_->factory, TRUE);
    
    // Allow both UDP and TCP transport so clients that can't receive UDP still work
    gst_rtsp_media_factory_set_protocols(gst_data_->factory,
        (GstRTSPLowerTrans)(GST_RTSP_LOWER_TRANS_UDP | GST_RTSP_LOWER_TRANS_TCP));
    
    // Connect to media-configure signal to get appsrc when client connects
    g_signal_connect(gst_data_->factory, "media-configure", G_CALLBACK(media_configure_cb), gst_data_.get());
    
    gst_rtsp_mount_points_add_factory(gst_data_->mounts, 
                                       config_.mount_point.c_str(), 
                                       gst_data_->factory);
    g_object_unref(gst_data_->mounts);
    
    // Create a new context for the RTSP server
    gst_data_->context = g_main_context_new();
    gst_rtsp_server_attach(gst_data_->server, gst_data_->context);
    
    running_ = true;
    gst_thread_ = std::thread(&RTSPServer::gst_thread_func, this);
    
    LOG_INFO("RTSP server started on port ", config_.port);
    LOG_INFO("Connect with: vlc rtsp://localhost:", config_.port, config_.mount_point);
    return true;
}

void RTSPServer::stop() {
    if (!running_) {
        return;
    }

    LOG_INFO("Stopping stream server");
    running_ = false;
    frame_cv_.notify_all();

    if (gst_data_->loop) {
        g_main_loop_quit(gst_data_->loop);
    }
    
    if (gst_thread_.joinable()) {
        gst_thread_.join();
    }
    
    // Clear appsrcs
    {
        std::lock_guard<std::mutex> lock(gst_data_->appsrc_mutex);
        gst_data_->appsrcs.clear();
        gst_data_->audio_appsrcs.clear();
    }
    
    if (gst_data_->context) {
        g_main_context_unref(gst_data_->context);
        gst_data_->context = nullptr;
    }
    
    if (gst_data_->server) {
        g_object_unref(gst_data_->server);
        gst_data_->server = nullptr;
    }

    LOG_INFO("Stream server stopped");
}

void RTSPServer::push_frame(const EncodedFrame& frame) {
    if (!running_) {
        return;
    }

    // Cache keyframes for instant playback when clients connect (zero-copy via shared_ptr)
    if (frame.metadata.is_keyframe) {
        std::lock_guard<std::mutex> kf_lock(gst_data_->keyframe_mutex);
        gst_data_->last_keyframe = std::make_shared<const std::vector<uint8_t>>(frame.data);
        gst_data_->keyframe_timestamp_us = frame.metadata.timestamp_us;
    }

    // Push frame to all connected GStreamer appsrcs
    std::lock_guard<std::mutex> lock(gst_data_->appsrc_mutex);
    
    if (gst_data_->appsrcs.empty()) {
        return;  // No clients connected
    }
    
    static uint64_t frame_count = 0;
    frame_count++;
    
    // Log periodically
    if (frame_count % 300 == 1) {
        LOG_INFO("RTSP pushing frame #", frame_count, ", size=", frame.data.size(), 
                 " bytes, keyframe=", frame.metadata.is_keyframe, 
                 ", clients=", gst_data_->appsrcs.size());
    }
    
    for (auto* appsrc : gst_data_->appsrcs) {
        // Create a GstBuffer from the frame data
        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, frame.data.size(), nullptr);
        
        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            memcpy(map.data, frame.data.data(), frame.data.size());
            gst_buffer_unmap(buffer, &map);
        }
        
        // Let do-timestamp handle timing for live streams
        GST_BUFFER_PTS(buffer) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;
        
        // Push buffer to appsrc
        GstFlowReturn ret = gst_app_src_push_buffer(appsrc, buffer);
        if (ret == GST_FLOW_OK) {
            frames_sent_++;
            bytes_sent_ += frame.data.size();
        } else if (frame_count % 300 == 1) {
            LOG_WARN("RTSP appsrc push failed: ", ret);
        }
    }
    
    client_count_ = gst_data_->appsrcs.size();
}

void RTSPServer::push_audio(const AudioReader::AudioChunk& chunk) {
    if (!running_ || !config_.enable_audio) {
        return;
    }

    std::lock_guard<std::mutex> lock(gst_data_->appsrc_mutex);

    if (gst_data_->audio_appsrcs.empty()) {
        return;
    }

    static uint64_t audio_push_count = 0;
    audio_push_count++;
    if (audio_push_count % 500 == 1) {
        LOG_INFO("RTSP pushing audio chunk #", audio_push_count,
                 ", size=", chunk.data.size(), " bytes, frames=", chunk.frame_count,
                 ", audio_appsrcs=", gst_data_->audio_appsrcs.size());
    }

    for (auto* audio_appsrc : gst_data_->audio_appsrcs) {
        GstBuffer* buffer = gst_buffer_new_allocate(nullptr, chunk.data.size(), nullptr);

        GstMapInfo map;
        if (gst_buffer_map(buffer, &map, GST_MAP_WRITE)) {
            memcpy(map.data, chunk.data.data(), chunk.data.size());
            gst_buffer_unmap(buffer, &map);
        }

        GST_BUFFER_PTS(buffer) = GST_CLOCK_TIME_NONE;
        GST_BUFFER_DTS(buffer) = GST_CLOCK_TIME_NONE;

        GstFlowReturn ret = gst_app_src_push_buffer(audio_appsrc, buffer);
        if (ret != GST_FLOW_OK && audio_push_count % 500 == 1) {
            LOG_WARN("RTSP audio appsrc push failed: ", ret);
        }
    }
}

void RTSPServer::set_audio_format(uint32_t sample_rate, uint16_t channels, uint16_t bits_per_sample) {
    if (gst_data_) {
        gst_data_->audio_sample_rate = sample_rate;
        gst_data_->audio_channels = channels;
        gst_data_->audio_bits = bits_per_sample;
        LOG_INFO("RTSP audio format set: ", sample_rate, " Hz, ", channels, " ch, ", bits_per_sample, " bit");
    }
}

std::string RTSPServer::get_url() const {
    return "rtsp://localhost:" + std::to_string(config_.port) + config_.mount_point;
}

size_t RTSPServer::client_count() const {
    return client_count_.load();
}

RTSPServer::Stats RTSPServer::get_stats() const {
    return {
        frames_sent_.load(),
        bytes_sent_.load(),
        client_count_.load()
    };
}

void RTSPServer::gst_thread_func() {
    LOG_DEBUG("GStreamer RTSP thread started");
    
    gst_data_->loop = g_main_loop_new(gst_data_->context, FALSE);
    
    // Run the main loop
    g_main_loop_run(gst_data_->loop);
    
    g_main_loop_unref(gst_data_->loop);
    gst_data_->loop = nullptr;
    
    LOG_DEBUG("GStreamer RTSP thread stopped");
}

} // namespace camera_daemon
