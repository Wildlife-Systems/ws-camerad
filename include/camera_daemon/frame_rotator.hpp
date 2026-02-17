#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>

namespace camera_daemon {

/**
 * Rotates YUV420 planar frames by 90 or 270 degrees.
 * 
 * For 0° and 180°, use libcamera's Orientation instead (ISP handles it free).
 * This class handles only 90°/270° which require a software transpose.
 * Optimized with ARM NEON SIMD for ~1-2ms at 1280x960 on Cortex-A72.
 *
 * YUV420 planar layout:
 *   Y plane: width * height bytes (one luma per pixel)
 *   U plane: (width/2) * (height/2) bytes
 *   V plane: (width/2) * (height/2) bytes
 */
class FrameRotator {
public:
    enum class Rotation {
        None = 0,
        Rot90 = 90,
        Rot180 = 180,  // Handled by ISP, not this class
        Rot270 = 270
    };

    /**
     * Create a rotator for the given source dimensions.
     * Pre-allocates the output buffer.
     */
    FrameRotator(uint32_t src_width, uint32_t src_height, Rotation rotation);

    /**
     * Rotate a YUV420 frame.
     * @param src Source YUV420 planar data (Y + U + V planes, contiguous)
     * @param src_stride Stride of the source Y plane (may differ from width due to alignment)
     * @return Pointer to rotated data (owned by this object, valid until next rotate() call)
     */
    const uint8_t* rotate(const uint8_t* src, uint32_t src_stride);

    /** @return Size of the rotated frame in bytes */
    size_t rotated_size() const { return dst_buf_.size(); }

    /** @return Width after rotation */
    uint32_t dst_width() const { return dst_width_; }

    /** @return Height after rotation */
    uint32_t dst_height() const { return dst_height_; }

    /** @return Stride of the rotated Y plane */
    uint32_t dst_stride() const { return dst_width_; }  // No padding needed for software buffer

    /** @return Total YUV420 size for dst dimensions */
    static size_t yuv420_size(uint32_t w, uint32_t h) {
        return w * h * 3 / 2;
    }

private:
    void rotate_plane(const uint8_t* src, uint8_t* dst,
                      uint32_t src_w, uint32_t src_h, uint32_t src_stride);
    
    void rotate_plane_90(const uint8_t* src, uint8_t* dst,
                         uint32_t src_w, uint32_t src_h, uint32_t src_stride);
    
    void rotate_plane_270(const uint8_t* src, uint8_t* dst,
                          uint32_t src_w, uint32_t src_h, uint32_t src_stride);

    Rotation rotation_;
    uint32_t src_width_;
    uint32_t src_height_;
    uint32_t dst_width_;
    uint32_t dst_height_;
    std::vector<uint8_t> dst_buf_;
};

} // namespace camera_daemon
