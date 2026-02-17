#include "camera_daemon/shared_memory.hpp"
#include "camera_daemon/logger.hpp"
#include <sys/mman.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <unistd.h>
#include <cstring>
#include <thread>

namespace camera_daemon {

SharedMemory::SharedMemory(const std::string& name, size_t size, bool create)
    : name_(name), size_(size), data_(nullptr), fd_(-1), owner_(create) {
    
    int flags = create ? (O_CREAT | O_RDWR) : O_RDWR;
    mode_t mode = S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP;
    
    fd_ = shm_open(name.c_str(), flags, mode);
    if (fd_ < 0) {
        LOG_ERROR("Failed to open shared memory '", name, "': ", strerror(errno));
        return;
    }
    
    if (create) {
        if (ftruncate(fd_, size) < 0) {
            LOG_ERROR("Failed to resize shared memory: ", strerror(errno));
            close(fd_);
            fd_ = -1;
            return;
        }
    }
    
    int prot = PROT_READ | (create ? PROT_WRITE : 0);
    data_ = mmap(nullptr, size, prot, MAP_SHARED, fd_, 0);
    if (data_ == MAP_FAILED) {
        LOG_ERROR("Failed to mmap shared memory: ", strerror(errno));
        data_ = nullptr;
        close(fd_);
        fd_ = -1;
        return;
    }
    
    LOG_DEBUG("Opened shared memory '", name, "': ", size, " bytes, ", (create ? "created" : "attached"));
}

SharedMemory::~SharedMemory() {
    if (data_) {
        munmap(data_, size_);
    }
    if (fd_ >= 0) {
        close(fd_);
    }
}

SharedMemory::SharedMemory(SharedMemory&& other) noexcept
    : name_(std::move(other.name_))
    , size_(other.size_)
    , data_(other.data_)
    , fd_(other.fd_)
    , owner_(other.owner_) {
    other.data_ = nullptr;
    other.fd_ = -1;
    other.size_ = 0;
}

SharedMemory& SharedMemory::operator=(SharedMemory&& other) noexcept {
    if (this != &other) {
        if (data_) munmap(data_, size_);
        if (fd_ >= 0) close(fd_);
        
        name_ = std::move(other.name_);
        size_ = other.size_;
        data_ = other.data_;
        fd_ = other.fd_;
        owner_ = other.owner_;
        
        other.data_ = nullptr;
        other.fd_ = -1;
        other.size_ = 0;
    }
    return *this;
}

void SharedMemory::unlink() {
    if (!name_.empty()) {
        shm_unlink(name_.c_str());
        LOG_DEBUG("Unlinked shared memory: ", name_);
    }
}

// FramePublisher implementation

FramePublisher::FramePublisher(const std::string& shm_name, uint32_t width, uint32_t height, uint32_t format) {
    // Calculate size based on format
    size_t frame_size;
    if (format == PIXFMT_BGR24) {
        frame_size = width * height * 3;  // BGR24: 3 bytes per pixel
    } else {
        frame_size = width * height * 3 / 2;  // YUV420/NV12: 1.5 bytes per pixel
    }
    size_t total_size = sizeof(Header) + frame_size;
    
    shm_ = std::make_unique<SharedMemory>(shm_name, total_size, true);
    if (!shm_->is_valid()) {
        LOG_ERROR("Failed to create shared memory for FramePublisher");
        return;
    }
    
    header_ = static_cast<Header*>(shm_->data());
    frame_data_ = reinterpret_cast<uint8_t*>(header_) + sizeof(Header);
    
    // Initialize header
    header_->sequence.store(0);
    header_->frame_ready.store(0);
    header_->width = width;
    header_->height = height;
    header_->stride = width;
    header_->format = format;
    header_->frame_size = frame_size;
    header_->timestamp_us = 0;
    header_->is_keyframe = 0;
    memset(header_->reserved, 0, sizeof(header_->reserved));
    
    LOG_INFO("Created frame publisher: ", width, "x", height, ", shm=", shm_name);
}

FramePublisher::~FramePublisher() {
    if (shm_) {
        shm_->unlink();
    }
}

void FramePublisher::publish(const uint8_t* data, size_t size, const FrameMetadata& metadata) {
    if (!shm_ || !shm_->is_valid()) {
        return;
    }
    
    // Mark as not ready during update
    header_->frame_ready.store(0, std::memory_order_release);
    
    // Copy metadata
    header_->timestamp_us = metadata.timestamp_us;
    header_->width = metadata.width;
    header_->height = metadata.height;
    header_->is_keyframe = metadata.is_keyframe ? 1 : 0;
    
    // Copy frame data
    size_t copy_size = std::min(size, static_cast<size_t>(header_->frame_size));
    memcpy(frame_data_, data, copy_size);
    
    // Update sequence and mark ready
    header_->sequence.fetch_add(1, std::memory_order_release);
    header_->frame_ready.store(1, std::memory_order_release);
}

uint64_t FramePublisher::sequence() const {
    return header_ ? header_->sequence.load(std::memory_order_acquire) : 0;
}

// FrameSubscriber implementation

FrameSubscriber::FrameSubscriber(const std::string& shm_name)
    : last_sequence_(0) {
    // Open shm file descriptor to get actual size via fstat
    int fd = shm_open(shm_name.c_str(), O_RDONLY, 0);
    if (fd < 0) {
        LOG_ERROR("Failed to open shared memory for FrameSubscriber: ", strerror(errno));
        return;
    }
    
    // Get actual file size - avoids double mapping
    struct stat st;
    if (fstat(fd, &st) < 0) {
        LOG_ERROR("Failed to stat shared memory: ", strerror(errno));
        close(fd);
        return;
    }
    
    size_t full_size = st.st_size;
    close(fd);  // SharedMemory will reopen it
    
    if (full_size < sizeof(FramePublisher::Header)) {
        LOG_ERROR("Shared memory too small");
        return;
    }
    
    shm_ = std::make_unique<SharedMemory>(shm_name, full_size, false);
    if (!shm_->is_valid()) {
        LOG_ERROR("Failed to open shared memory for FrameSubscriber");
        return;
    }
    
    header_ = static_cast<const FramePublisher::Header*>(shm_->data());
    frame_data_ = reinterpret_cast<const uint8_t*>(header_) + sizeof(FramePublisher::Header);
    
    LOG_INFO("Created frame subscriber: ", header_->width, "x", header_->height);
}

FrameSubscriber::~FrameSubscriber() = default;

bool FrameSubscriber::read_frame(std::vector<uint8_t>& data, FrameMetadata& metadata, int timeout_ms) {
    if (!shm_ || !shm_->is_valid()) {
        return false;
    }
    
    auto start = std::chrono::steady_clock::now();
    
    while (true) {
        uint64_t seq = header_->sequence.load(std::memory_order_acquire);
        if (seq > last_sequence_ && header_->frame_ready.load(std::memory_order_acquire)) {
            // New frame available
            metadata.timestamp_us = header_->timestamp_us;
            metadata.sequence = seq;
            metadata.width = header_->width;
            metadata.height = header_->height;
            metadata.stride = header_->stride;
            metadata.size = header_->frame_size;
            metadata.format = header_->format;
            metadata.is_keyframe = header_->is_keyframe != 0;
            
            data.resize(header_->frame_size);
            memcpy(data.data(), frame_data_, header_->frame_size);
            
            // Verify sequence didn't change during copy
            if (header_->sequence.load(std::memory_order_acquire) == seq) {
                last_sequence_ = seq;
                return true;
            }
            // Frame changed during copy, try again
            continue;
        }
        
        if (timeout_ms == 0) {
            return false;
        }
        
        if (timeout_ms > 0) {
            auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - start
            ).count();
            if (elapsed >= timeout_ms) {
                return false;
            }
        }
        
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }
}

bool FrameSubscriber::has_new_frame() const {
    if (!header_) return false;
    return header_->sequence.load(std::memory_order_acquire) > last_sequence_ &&
           header_->frame_ready.load(std::memory_order_acquire);
}

uint32_t FrameSubscriber::width() const {
    return header_ ? header_->width : 0;
}

uint32_t FrameSubscriber::height() const {
    return header_ ? header_->height : 0;
}

} // namespace camera_daemon
