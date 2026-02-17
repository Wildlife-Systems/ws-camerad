#include "camera_daemon/frame_rotator.hpp"
#include "camera_daemon/logger.hpp"
#include <cstring>
#include <future>

#ifdef __ARM_NEON
#include <arm_neon.h>
#endif

namespace camera_daemon {

FrameRotator::FrameRotator(uint32_t src_width, uint32_t src_height, Rotation rotation)
    : rotation_(rotation)
    , src_width_(src_width)
    , src_height_(src_height)
{
    if (rotation == Rotation::Rot90 || rotation == Rotation::Rot270) {
        // Dimensions swap
        dst_width_ = src_height;
        dst_height_ = src_width;
    } else {
        dst_width_ = src_width;
        dst_height_ = src_height;
    }

    dst_buf_.resize(yuv420_size(dst_width_, dst_height_));
    LOG_INFO("FrameRotator: ", src_width_, "x", src_height_, " -> ",
             dst_width_, "x", dst_height_, " (", static_cast<int>(rotation), "°)");
}

const uint8_t* FrameRotator::rotate(const uint8_t* src, uint32_t src_stride) {
    // Chroma dimensions
    uint32_t chroma_w = src_width_ / 2;
    uint32_t chroma_h = src_height_ / 2;
    uint32_t chroma_stride = src_stride / 2;

    // Y plane pointers
    const uint8_t* src_y = src;
    uint8_t* dst_y = dst_buf_.data();

    // U plane pointers
    const uint8_t* src_u = src + src_stride * src_height_;
    uint8_t* dst_u = dst_y + dst_width_ * dst_height_;

    // V plane pointers
    const uint8_t* src_v = src_u + chroma_stride * chroma_h;
    uint8_t* dst_v = dst_u + (dst_width_ / 2) * (dst_height_ / 2);

    // Rotate Y plane on a separate thread (biggest plane - 4x chroma)
    auto y_future = std::async(std::launch::async, [this, src_y, dst_y, src_stride]() {
        rotate_plane(src_y, dst_y, src_width_, src_height_, src_stride);
    });

    // Rotate U and V in parallel on current thread and another async
    // (They're small enough that spawning both async has overhead, 
    // so do V on main thread while Y runs async)
    auto u_future = std::async(std::launch::async, [this, src_u, dst_u, chroma_w, chroma_h, chroma_stride]() {
        rotate_plane(src_u, dst_u, chroma_w, chroma_h, chroma_stride);
    });

    // V on main thread
    rotate_plane(src_v, dst_v, chroma_w, chroma_h, chroma_stride);

    // Wait for Y and U to complete
    y_future.get();
    u_future.get();

    return dst_buf_.data();
}

void FrameRotator::rotate_plane(const uint8_t* src, uint8_t* dst,
                                 uint32_t src_w, uint32_t src_h, uint32_t src_stride) {
    if (rotation_ == Rotation::Rot90) {
        rotate_plane_90(src, dst, src_w, src_h, src_stride);
    } else {
        rotate_plane_270(src, dst, src_w, src_h, src_stride);
    }
}

/**
 * Rotate 90° clockwise:
 *   dst[x][h-1-y] = src[y][x]
 *   dst_stride = src_h (dst width = src_height)
 *
 * NEON optimization: process 8x8 blocks using transpose + reverse.
 * The 8x8 block approach is cache-friendly for both source (sequential rows)
 * and destination (sequential columns become sequential rows after transpose).
 */
void FrameRotator::rotate_plane_90(const uint8_t* src, uint8_t* dst,
                                    uint32_t src_w, uint32_t src_h, uint32_t src_stride) {
    uint32_t dst_stride = src_h;  // dst width = src_height

#ifdef __ARM_NEON
    // Process 8x8 blocks with NEON
    uint32_t bx, by;
    for (by = 0; by + 8 <= src_h; by += 8) {
        for (bx = 0; bx + 8 <= src_w; bx += 8) {
            // Prefetch next source block (2 blocks ahead in x direction)
            // This hides memory latency for sequential source reads
            if (bx + 16 <= src_w) {
                __builtin_prefetch(src + (by + 0) * src_stride + bx + 16, 0, 3);
                __builtin_prefetch(src + (by + 4) * src_stride + bx + 16, 0, 3);
            } else if (by + 16 <= src_h) {
                // Prefetch first block of next-next row
                __builtin_prefetch(src + (by + 8) * src_stride, 0, 3);
                __builtin_prefetch(src + (by + 12) * src_stride, 0, 3);
            }
            
            // Prefetch destination cache lines (write allocate hint)
            // Destination writes are strided by dst_stride, so prefetch alternating lines
            uint8_t* dst_pf = dst + (bx + 8) * dst_stride + (src_h - 1 - by);
            if (bx + 8 <= src_w) {
                __builtin_prefetch(dst_pf - 7, 1, 3);
                __builtin_prefetch(dst_pf - 7 + 4 * dst_stride, 1, 3);
            }
            
            // Load 8 rows of 8 bytes each
            uint8x8_t r0 = vld1_u8(src + (by + 0) * src_stride + bx);
            uint8x8_t r1 = vld1_u8(src + (by + 1) * src_stride + bx);
            uint8x8_t r2 = vld1_u8(src + (by + 2) * src_stride + bx);
            uint8x8_t r3 = vld1_u8(src + (by + 3) * src_stride + bx);
            uint8x8_t r4 = vld1_u8(src + (by + 4) * src_stride + bx);
            uint8x8_t r5 = vld1_u8(src + (by + 5) * src_stride + bx);
            uint8x8_t r6 = vld1_u8(src + (by + 6) * src_stride + bx);
            uint8x8_t r7 = vld1_u8(src + (by + 7) * src_stride + bx);

            // Transpose 8x8 using interleave operations
            // Step 1: Interleave pairs of rows (8-bit -> 16-bit pairs)
            uint8x8x2_t t01 = vtrn_u8(r0, r1);
            uint8x8x2_t t23 = vtrn_u8(r2, r3);
            uint8x8x2_t t45 = vtrn_u8(r4, r5);
            uint8x8x2_t t67 = vtrn_u8(r6, r7);

            // Step 2: Interleave 16-bit pairs
            uint16x4x2_t u02 = vtrn_u16(vreinterpret_u16_u8(t01.val[0]),
                                          vreinterpret_u16_u8(t23.val[0]));
            uint16x4x2_t u13 = vtrn_u16(vreinterpret_u16_u8(t01.val[1]),
                                          vreinterpret_u16_u8(t23.val[1]));
            uint16x4x2_t u46 = vtrn_u16(vreinterpret_u16_u8(t45.val[0]),
                                          vreinterpret_u16_u8(t67.val[0]));
            uint16x4x2_t u57 = vtrn_u16(vreinterpret_u16_u8(t45.val[1]),
                                          vreinterpret_u16_u8(t67.val[1]));

            // Step 3: Interleave 32-bit pairs
            uint32x2x2_t v04 = vtrn_u32(vreinterpret_u32_u16(u02.val[0]),
                                          vreinterpret_u32_u16(u46.val[0]));
            uint32x2x2_t v26 = vtrn_u32(vreinterpret_u32_u16(u02.val[1]),
                                          vreinterpret_u32_u16(u46.val[1]));
            uint32x2x2_t v15 = vtrn_u32(vreinterpret_u32_u16(u13.val[0]),
                                          vreinterpret_u32_u16(u57.val[0]));
            uint32x2x2_t v37 = vtrn_u32(vreinterpret_u32_u16(u13.val[1]),
                                          vreinterpret_u32_u16(u57.val[1]));

            // Now we have the transposed 8x8 block in v04, v15, v26, v37
            // For 90° CW rotation: transpose + reverse rows
            // dst[col][dst_stride - 1 - row] = src[row][col]
            // After transpose, reverse the column order (write bottom to top)
            
            // Destination column = bx, row starts at (src_h - 1 - by) going down by 1
            // In transposed form: write row i of transposed block to 
            //   dst[(bx + i) * dst_stride + (src_h - 1 - by - 7)] through (src_h - 1 - by)
            // But we need to reverse each 8-byte vector
            
            uint8_t* dst_base = dst + bx * dst_stride + (src_h - 1 - by);

            // Reverse each transposed row and store
            // Row 0 of transposed = column 0 of original, reversed for 90° CW
            uint8x8_t tr0 = vrev64_u8(vreinterpret_u8_u32(v04.val[0]));
            uint8x8_t tr1 = vrev64_u8(vreinterpret_u8_u32(v15.val[0]));
            uint8x8_t tr2 = vrev64_u8(vreinterpret_u8_u32(v26.val[0]));
            uint8x8_t tr3 = vrev64_u8(vreinterpret_u8_u32(v37.val[0]));
            uint8x8_t tr4 = vrev64_u8(vreinterpret_u8_u32(v04.val[1]));
            uint8x8_t tr5 = vrev64_u8(vreinterpret_u8_u32(v15.val[1]));
            uint8x8_t tr6 = vrev64_u8(vreinterpret_u8_u32(v26.val[1]));
            uint8x8_t tr7 = vrev64_u8(vreinterpret_u8_u32(v37.val[1]));

            // Store: each row goes to dst at (bx+i) * dst_stride, starting at column (src_h-1-by-7)
            // Since we reversed, we store starting at (src_h - 1 - by - 7)
            vst1_u8(dst_base - 7 + 0 * dst_stride, tr0);
            vst1_u8(dst_base - 7 + 1 * dst_stride, tr1);
            vst1_u8(dst_base - 7 + 2 * dst_stride, tr2);
            vst1_u8(dst_base - 7 + 3 * dst_stride, tr3);
            vst1_u8(dst_base - 7 + 4 * dst_stride, tr4);
            vst1_u8(dst_base - 7 + 5 * dst_stride, tr5);
            vst1_u8(dst_base - 7 + 6 * dst_stride, tr6);
            vst1_u8(dst_base - 7 + 7 * dst_stride, tr7);
        }

        // Handle remaining columns (src_w not multiple of 8)
        for (uint32_t x = bx; x < src_w; x++) {
            for (uint32_t y = by; y < by + 8 && y < src_h; y++) {
                dst[x * dst_stride + (src_h - 1 - y)] = src[y * src_stride + x];
            }
        }
    }

    // Handle remaining rows (src_h not multiple of 8)
    for (uint32_t y = by; y < src_h; y++) {
        for (uint32_t x = 0; x < src_w; x++) {
            dst[x * dst_stride + (src_h - 1 - y)] = src[y * src_stride + x];
        }
    }
#else
    // Scalar fallback
    for (uint32_t y = 0; y < src_h; y++) {
        for (uint32_t x = 0; x < src_w; x++) {
            dst[x * dst_stride + (src_h - 1 - y)] = src[y * src_stride + x];
        }
    }
#endif
}

/**
 * Rotate 270° clockwise (= 90° counter-clockwise):
 *   dst[w-1-x][y] = src[y][x]
 *   dst_stride = src_h
 */
void FrameRotator::rotate_plane_270(const uint8_t* src, uint8_t* dst,
                                     uint32_t src_w, uint32_t src_h, uint32_t src_stride) {
    uint32_t dst_stride = src_h;  // dst width = src_height

#ifdef __ARM_NEON
    uint32_t bx, by;
    for (by = 0; by + 8 <= src_h; by += 8) {
        for (bx = 0; bx + 8 <= src_w; bx += 8) {
            // Prefetch next source block (2 blocks ahead in x direction)
            if (bx + 16 <= src_w) {
                __builtin_prefetch(src + (by + 0) * src_stride + bx + 16, 0, 3);
                __builtin_prefetch(src + (by + 4) * src_stride + bx + 16, 0, 3);
            } else if (by + 16 <= src_h) {
                __builtin_prefetch(src + (by + 8) * src_stride, 0, 3);
                __builtin_prefetch(src + (by + 12) * src_stride, 0, 3);
            }
            
            // Prefetch destination cache lines
            uint8_t* dst_pf = dst + (src_w - 1 - bx - 8) * dst_stride + by;
            if (bx + 8 <= src_w) {
                __builtin_prefetch(dst_pf, 1, 3);
                __builtin_prefetch(dst_pf - 4 * dst_stride, 1, 3);
            }
            
            // Load 8 rows of 8 bytes
            uint8x8_t r0 = vld1_u8(src + (by + 0) * src_stride + bx);
            uint8x8_t r1 = vld1_u8(src + (by + 1) * src_stride + bx);
            uint8x8_t r2 = vld1_u8(src + (by + 2) * src_stride + bx);
            uint8x8_t r3 = vld1_u8(src + (by + 3) * src_stride + bx);
            uint8x8_t r4 = vld1_u8(src + (by + 4) * src_stride + bx);
            uint8x8_t r5 = vld1_u8(src + (by + 5) * src_stride + bx);
            uint8x8_t r6 = vld1_u8(src + (by + 6) * src_stride + bx);
            uint8x8_t r7 = vld1_u8(src + (by + 7) * src_stride + bx);

            // Transpose 8x8 (same as 90°)
            uint8x8x2_t t01 = vtrn_u8(r0, r1);
            uint8x8x2_t t23 = vtrn_u8(r2, r3);
            uint8x8x2_t t45 = vtrn_u8(r4, r5);
            uint8x8x2_t t67 = vtrn_u8(r6, r7);

            uint16x4x2_t u02 = vtrn_u16(vreinterpret_u16_u8(t01.val[0]),
                                          vreinterpret_u16_u8(t23.val[0]));
            uint16x4x2_t u13 = vtrn_u16(vreinterpret_u16_u8(t01.val[1]),
                                          vreinterpret_u16_u8(t23.val[1]));
            uint16x4x2_t u46 = vtrn_u16(vreinterpret_u16_u8(t45.val[0]),
                                          vreinterpret_u16_u8(t67.val[0]));
            uint16x4x2_t u57 = vtrn_u16(vreinterpret_u16_u8(t45.val[1]),
                                          vreinterpret_u16_u8(t67.val[1]));

            uint32x2x2_t v04 = vtrn_u32(vreinterpret_u32_u16(u02.val[0]),
                                          vreinterpret_u32_u16(u46.val[0]));
            uint32x2x2_t v26 = vtrn_u32(vreinterpret_u32_u16(u02.val[1]),
                                          vreinterpret_u32_u16(u46.val[1]));
            uint32x2x2_t v15 = vtrn_u32(vreinterpret_u32_u16(u13.val[0]),
                                          vreinterpret_u32_u16(u57.val[0]));
            uint32x2x2_t v37 = vtrn_u32(vreinterpret_u32_u16(u13.val[1]),
                                          vreinterpret_u32_u16(u57.val[1]));

            // For 270° CW: transpose + reverse columns
            // dst[(src_w - 1 - x) * dst_stride + y] = src[y][x]
            // Destination base: row = (src_w - 1 - bx), column = by
            uint8_t* dst_base = dst + (src_w - 1 - bx) * dst_stride + by;

            // Store transposed rows (no reverse needed, but write top-to-bottom reversed)
            // Row i of transposed block goes to dst row (src_w - 1 - bx - i)
            uint8x8_t tr0 = vreinterpret_u8_u32(v04.val[0]);
            uint8x8_t tr1 = vreinterpret_u8_u32(v15.val[0]);
            uint8x8_t tr2 = vreinterpret_u8_u32(v26.val[0]);
            uint8x8_t tr3 = vreinterpret_u8_u32(v37.val[0]);
            uint8x8_t tr4 = vreinterpret_u8_u32(v04.val[1]);
            uint8x8_t tr5 = vreinterpret_u8_u32(v15.val[1]);
            uint8x8_t tr6 = vreinterpret_u8_u32(v26.val[1]);
            uint8x8_t tr7 = vreinterpret_u8_u32(v37.val[1]);

            // Write rows in reverse order (row 0 of transposed -> bottom of dst block)
            vst1_u8(dst_base                  , tr0);
            vst1_u8(dst_base - 1 * dst_stride, tr1);
            vst1_u8(dst_base - 2 * dst_stride, tr2);
            vst1_u8(dst_base - 3 * dst_stride, tr3);
            vst1_u8(dst_base - 4 * dst_stride, tr4);
            vst1_u8(dst_base - 5 * dst_stride, tr5);
            vst1_u8(dst_base - 6 * dst_stride, tr6);
            vst1_u8(dst_base - 7 * dst_stride, tr7);
        }

        // Remaining columns
        for (uint32_t x = bx; x < src_w; x++) {
            for (uint32_t y = by; y < by + 8 && y < src_h; y++) {
                dst[(src_w - 1 - x) * dst_stride + y] = src[y * src_stride + x];
            }
        }
    }

    // Remaining rows
    for (uint32_t y = by; y < src_h; y++) {
        for (uint32_t x = 0; x < src_w; x++) {
            dst[(src_w - 1 - x) * dst_stride + y] = src[y * src_stride + x];
        }
    }
#else
    // Scalar fallback
    for (uint32_t y = 0; y < src_h; y++) {
        for (uint32_t x = 0; x < src_w; x++) {
            dst[(src_w - 1 - x) * dst_stride + y] = src[y * src_stride + x];
        }
    }
#endif
}

} // namespace camera_daemon
