#pragma once

#include "imgui.h"

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace app_gui {

// Width of the resize border around the borderless window, at 100% display scale.
constexpr float kHit = 8.0f;

struct Stats {
    double fps = 0.0;
    double app_ms = 0.0;
    uint64_t source_gaps = 0;
    uint64_t latest_drops = 0;
    uint64_t present_skips = 0;
    double mister_ms = 0.0;
    double decode_ms = 0.0; // MJPEG only
    uint64_t mister_sent = 0;
    uint64_t mister_replaced = 0;
    uint64_t mister_errors = 0;
    std::string mister_error;
    bool mister_connected = false;
    bool mister_calibrating = false;
    long allocator_buffers = -1;
    uint32_t audio_rate_hz = 0;
    std::string audio_error;
    std::string capture_error;
    std::string settings_error;
    std::string capture_warning; // a hint while the stream runs, not a failure
};

// Everything the panel shows, and what the user asked for since the last frame.
struct Frame {
    float scale = 1.0f; // the display's scale: 1 at 100%
    bool fullscreen = false;
    bool letterbox = false;
    bool show_stats = false;
    int crop_h = 0; // percent: + crop capture, - add black bars
    int crop_v = 0;
    bool stream = false;
    bool has_video = false;
    bool has_capture = false; // a capture device is open
    bool pal = false;
    bool progressive = false;
    int device_index = 0;
    int audio_index = 0;
    int pixel_format_index = 0;
    int size_index = 0;
    int rate_index = 0;
    int default_size_index = -1; // the device's default, if listed
    int default_rate_index = -1;
    char ip[64]{};
    std::vector<std::string> devices;
    std::vector<std::string> audio_devices;
    std::vector<std::string> pixel_formats;
    std::vector<std::string> sizes;
    std::vector<std::string> rates;
    Stats stats;

    bool close = false;
    bool minimize = false;
    bool toggle_maximize = false;
    bool device_changed = false;
    bool audio_changed = false;
    bool refresh_devices = false;
    bool devices_changed = false; // Windows reported hardware coming or going
    bool scale_changed = false;   // the window moved to a display with another scale
    bool format_changed = false;
    bool ip_changed = false;

    // Layout, set by draw().
    float video_x = 0.0f;
    float video_y = 0.0f;
    float video_w = 1.0f;
    float video_h = 1.0f;
    ImVec2 caption_min{};
    ImVec2 caption_max{};
};

void apply_theme(float scale);
void draw(Frame& frame);

} // namespace app_gui
