#pragma once

#include <cmath>

namespace groovy {

// 15 kHz modes only: the target CRTs cannot sync to 31 kHz.
enum class VideoMode { p240, i480, p288, i576 };

inline bool is_interlaced(VideoMode mode) {
    return mode == VideoMode::i480 || mode == VideoMode::i576;
}

// Lines per blit (one field in interlaced modes).
inline unsigned output_height(VideoMode mode) {
    return mode == VideoMode::p288 || mode == VideoMode::i576 ? 288 : 240;
}

inline bool is_pal_mode(VideoMode mode) {
    return mode == VideoMode::p288 || mode == VideoMode::i576;
}

inline bool is_pal_fps(double fps) {
    return std::abs(fps - 100.0) < 2.0 || std::abs(fps - 50.0) < 2.0 || std::abs(fps - 25.0) < 2.0;
}

inline VideoMode video_mode_for(bool pal, bool progressive) {
    if (pal) return progressive ? VideoMode::p288 : VideoMode::i576;
    return progressive ? VideoMode::p240 : VideoMode::i480;
}

inline const char* video_mode_name(VideoMode mode) {
    switch (mode) {
    case VideoMode::p240: return "240p";
    case VideoMode::i480: return "480i";
    case VideoMode::p288: return "288p";
    case VideoMode::i576: return "576i";
    }
    return "unknown";
}

} // namespace groovy
