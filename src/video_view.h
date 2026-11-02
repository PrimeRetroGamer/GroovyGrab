#pragma once

#include <algorithm>

// Shared capture -> CRT window. Letterbox is a 16:9 content box inside the 4:3
// raster. Crop is independent: positive cuts into the capture, negative adds black
// bars. Applied during conversion and in the preview.
namespace video_view {

constexpr int kMaxCropPercent = 25;

struct Mapping {
    unsigned src_x0, src_y0, src_x1, src_y1;
    unsigned dst_x0, dst_y0, dst_x1, dst_y1;
};

// Pulls [low, high) in from both ends by `percent` of its span, split between them.
inline void cut_edges(unsigned& low, unsigned& high, int percent) {
    const unsigned cut = (high - low) * static_cast<unsigned>(percent) / 200u;
    low += cut;
    high -= cut;
}

inline Mapping make(unsigned src_w, unsigned src_h, unsigned dst_w, unsigned dst_h,
                    bool letterbox, int crop_h, int crop_v) {
    Mapping m{0, 0, src_w, src_h, 0, 0, dst_w, dst_h};
    crop_h = std::clamp(crop_h, -kMaxCropPercent, kMaxCropPercent);
    crop_v = std::clamp(crop_v, -kMaxCropPercent, kMaxCropPercent);

    if (letterbox) {
        const unsigned bar = dst_h / 8u;
        m.dst_y0 = bar;
        m.dst_y1 = dst_h - bar;
    }
    if (crop_h > 0) cut_edges(m.src_x0, m.src_x1, crop_h);
    else cut_edges(m.dst_x0, m.dst_x1, -crop_h);
    if (crop_v > 0) cut_edges(m.src_y0, m.src_y1, crop_v);
    else cut_edges(m.dst_y0, m.dst_y1, -crop_v);

    // Packed 4:2:2 and 4:2:0 chroma is shared by pairs of pixels.
    m.src_x0 &= ~1u;
    m.src_x1 &= ~1u;
    return m;
}

} // namespace video_view
