#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>

namespace camera_daemon {

/**
 * Hardware JPEG encoder using bcm2835-codec on Raspberry Pi.
 * Uses /dev/video31 for M2M JPEG encoding.
 */
class V4L2JpegEncoder {
public:
    struct Config {
        uint32_t width = 1280;
        uint32_t height = 960;
        int quality = 90;  // 1-100
    };

    V4L2JpegEncoder();
    ~V4L2JpegEncoder();

    // Non-copyable (only one can own the hardware)
    V4L2JpegEncoder(const V4L2JpegEncoder&) = delete;
    V4L2JpegEncoder& operator=(const V4L2JpegEncoder&) = delete;

    /**
     * Initialize the hardware JPEG encoder.
     */
    bool initialize(const Config& config);

    /**
     * Encode a YUV420 frame to JPEG.
     * @param yuv_data Pointer to YUV420 planar data
     * @param yuv_size Size of YUV data in bytes
     * @param jpeg_out Output vector to receive JPEG data
     * @return true on success
     */
    bool encode(const uint8_t* yuv_data, size_t yuv_size, std::vector<uint8_t>& jpeg_out);

    /**
     * Check if hardware JPEG is available.
     */
    static bool is_available();

private:
    bool open_device();
    bool setup_formats();
    bool allocate_buffers();
    bool stream_on();
    void stream_off();

    int fd_ = -1;
    Config config_;
    bool streaming_ = false;

    struct Buffer {
        void* data = nullptr;
        size_t capacity = 0;
    };

    Buffer input_buffer_;
    Buffer output_buffer_;
};

} // namespace camera_daemon
