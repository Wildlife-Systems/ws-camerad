#include "camera_daemon/rtsp_server.hpp"
#include "camera_daemon/logger.hpp"
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <cstring>
#include <memory>

#ifdef HAVE_GSTREAMER
#include <gst/gst.h>
#include <gst/rtsp-server/rtsp-server.h>
#include <gst/app/gstappsrc.h>
#endif

namespace camera_daemon {

#ifdef HAVE_GSTREAMER
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
    
    gst_object_unref(element);
}
} // anonymous namespace

// Wrapper struct that wraps the impl
struct RTSPServer::GstData : public GstDataImpl {};
#endif

RTSPServer::RTSPServer(const Config& config) : config_(config) {
#ifdef HAVE_GSTREAMER
    gst_data_ = std::make_unique<GstData>();
    gst_data_->owner = this;
#endif
}

RTSPServer::~RTSPServer() {
    stop();
}

bool RTSPServer::start() {
    if (running_) {
        return true;
    }

#ifdef HAVE_GSTREAMER
    gst_init(nullptr, nullptr);
    
    gst_data_->server = gst_rtsp_server_new();
    gst_rtsp_server_set_service(gst_data_->server, std::to_string(config_.port).c_str());
    
    gst_data_->mounts = gst_rtsp_server_get_mount_points(gst_data_->server);
    gst_data_->factory = gst_rtsp_media_factory_new();
    
    // Create launch string with appsrc
    // Minimal queue for low-latency live streaming
    std::string launch = "( appsrc name=source is-live=true format=time ! "
                         "queue max-size-buffers=1 max-size-time=0 max-size-bytes=0 leaky=downstream ! "
                         "h264parse ! rtph264pay name=pay0 pt=96 config-interval=1 )";
    
    gst_rtsp_media_factory_set_launch(gst_data_->factory, launch.c_str());
    gst_rtsp_media_factory_set_shared(gst_data_->factory, TRUE);
    
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
#else
    // Simple TCP-based fallback (raw H.264 streaming)
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0) {
        LOG_ERROR("Failed to create server socket: ", strerror(errno));
        return false;
    }

    int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(config_.port);

    if (bind(server_fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
        LOG_ERROR("Failed to bind server socket: ", strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    if (listen(server_fd_, 5) < 0) {
        LOG_ERROR("Failed to listen on server socket: ", strerror(errno));
        close(server_fd_);
        server_fd_ = -1;
        return false;
    }

    // Set non-blocking
    int flags = fcntl(server_fd_, F_GETFL, 0);
    fcntl(server_fd_, F_SETFL, flags | O_NONBLOCK);

    running_ = true;
    server_thread_ = std::thread(&RTSPServer::server_thread_func, this);

    LOG_INFO("Stream server started on port ", config_.port, " (raw H.264 over TCP)");
    LOG_INFO("Connect with: ffplay tcp://", "localhost:", config_.port);
    return true;
#endif
}

void RTSPServer::stop() {
    if (!running_) {
        return;
    }

    LOG_INFO("Stopping stream server");
    running_ = false;
    frame_cv_.notify_all();

#ifdef HAVE_GSTREAMER
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
    }
    
    if (gst_data_->context) {
        g_main_context_unref(gst_data_->context);
        gst_data_->context = nullptr;
    }
    
    if (gst_data_->server) {
        g_object_unref(gst_data_->server);
        gst_data_->server = nullptr;
    }
#else
    if (server_fd_ >= 0) {
        shutdown(server_fd_, SHUT_RDWR);
        close(server_fd_);
        server_fd_ = -1;
    }

    if (server_thread_.joinable()) {
        server_thread_.join();
    }

    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        for (int fd : client_fds_) {
            close(fd);
        }
        client_fds_.clear();
        
        for (auto& t : client_threads_) {
            if (t.joinable()) {
                t.join();
            }
        }
        client_threads_.clear();
    }
#endif

    LOG_INFO("Stream server stopped");
}

void RTSPServer::push_frame(const EncodedFrame& frame) {
    if (!running_) {
        return;
    }

#ifdef HAVE_GSTREAMER
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
#else
    {
        std::lock_guard<std::mutex> lock(frame_mutex_);
        
        // Limit queue size - O(1) with deque
        while (frame_queue_.size() >= MAX_QUEUE_SIZE) {
            frame_queue_.pop_front();
        }
        
        frame_queue_.push_back(frame);
    }
    frame_cv_.notify_all();
#endif
}

std::string RTSPServer::get_url() const {
#ifdef HAVE_GSTREAMER
    return "rtsp://localhost:" + std::to_string(config_.port) + config_.mount_point;
#else
    return "tcp://localhost:" + std::to_string(config_.port);
#endif
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

#ifndef HAVE_GSTREAMER

void RTSPServer::server_thread_func() {
    LOG_DEBUG("Stream server thread started");

    while (running_) {
        pollfd pfd{};
        pfd.fd = server_fd_;
        pfd.events = POLLIN;

        int ret = poll(&pfd, 1, 100);
        if (ret <= 0 || !running_) {
            continue;
        }

        int client_fd = accept(server_fd_, nullptr, nullptr);
        if (client_fd < 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_ERROR("Accept failed: ", strerror(errno));
            }
            continue;
        }

        LOG_INFO("Stream client connected");
        client_count_++;

        {
            std::lock_guard<std::mutex> lock(clients_mutex_);
            client_fds_.push_back(client_fd);
            client_threads_.emplace_back(&RTSPServer::client_thread_func, this, client_fd);
        }
    }

    LOG_DEBUG("Stream server thread stopped");
}

void RTSPServer::client_thread_func(int client_fd) {
    uint64_t last_sent_seq = 0;

    while (running_) {
        EncodedFrame frame;
        
        {
            std::unique_lock<std::mutex> lock(frame_mutex_);
            frame_cv_.wait_for(lock, std::chrono::milliseconds(100), [this, last_sent_seq]() {
                return !running_ || (!frame_queue_.empty() && 
                       frame_queue_.back().metadata.sequence > last_sent_seq);
            });

            if (!running_) {
                break;
            }

            if (frame_queue_.empty()) {
                continue;
            }

            // Find the next frame to send (prefer keyframes for new connections)
            bool found = false;
            for (const auto& f : frame_queue_) {
                if (f.metadata.sequence > last_sent_seq) {
                    if (last_sent_seq == 0 && !f.metadata.is_keyframe) {
                        continue;  // Wait for keyframe for new clients
                    }
                    frame = f;
                    found = true;
                    break;
                }
            }

            if (!found) {
                continue;
            }
        }

        // Send frame
        ssize_t written = write(client_fd, frame.data.data(), frame.data.size());
        if (written <= 0) {
            if (errno != EAGAIN && errno != EWOULDBLOCK) {
                LOG_DEBUG("Client disconnected");
                break;
            }
            continue;
        }

        last_sent_seq = frame.metadata.sequence;
        frames_sent_++;
        bytes_sent_ += written;
    }

    // Remove from client list
    {
        std::lock_guard<std::mutex> lock(clients_mutex_);
        auto it = std::find(client_fds_.begin(), client_fds_.end(), client_fd);
        if (it != client_fds_.end()) {
            client_fds_.erase(it);
        }
    }

    close(client_fd);
    client_count_--;
    LOG_INFO("Stream client disconnected");
}

#else // HAVE_GSTREAMER

void RTSPServer::gst_thread_func() {
    LOG_DEBUG("GStreamer RTSP thread started");
    
    gst_data_->loop = g_main_loop_new(gst_data_->context, FALSE);
    
    // Run the main loop
    g_main_loop_run(gst_data_->loop);
    
    g_main_loop_unref(gst_data_->loop);
    gst_data_->loop = nullptr;
    
    LOG_DEBUG("GStreamer RTSP thread stopped");
}

#endif // HAVE_GSTREAMER

} // namespace camera_daemon
