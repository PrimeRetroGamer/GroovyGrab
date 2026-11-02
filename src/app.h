#pragma once

#include <windows.h>

#include "app_gui.h"
#include "async_sender.h"
#include "audio_capture.h"
#include "directshow_capture.h"
#include "format_menu.h"
#include "jpeg_decode.h"
#include "preview_renderer.h"
#include "settings.h"

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

struct CommandLine {
    std::optional<std::wstring> device;
    std::optional<bool> progressive;
    bool letterbox = false;
    bool send = false;
    bool print_stats = false;
    bool rate_lock = true;
};

// Owns capture, preview and the MiSTer stream, and runs the window's main loop.
class App {
public:
    App(HWND hwnd, app_gui::Frame& gui, const CommandLine& command_line, const AppSettings& saved);
    ~App();

    void run();

private:
    using clock = std::chrono::steady_clock;

    bool pump_messages();
    bool poll_capture(DWORD timeout_ms);
    void refresh_mjpeg_preview(bool new_sample);
    void upload_planar_preview();
    void manage_stream();
    void draw_frame(bool from_main_loop);
    void handle_gui_requests();
    void update_statistics();
    void save_settings_if_changed();
    void remember_device();
    void remember_format();
    void use_saved_format();

    void rebuild_fonts();
    void update_devices();
    void open_capture();
    void open_audio();
    void close_capture();
    groovy::VideoMode wanted_mode() const;

    HWND hwnd_;
    app_gui::Frame& gui_;
    bool print_stats_;

    PreviewRenderer renderer_;
    FormatMenu menu_;

    const std::wstring ini_path_;
    AppSettings settings_; // what to keep: a device or format only changes by the user's choice
    AppSettings written_;  // what the file holds
    const bool device_forced_; // named on the command line, so the saved one doesn't take over
    // Set on the command line and not changed since, so not saved.
    std::optional<bool> progressive_override_;
    std::optional<bool> letterbox_override_;
    clock::time_point recheck_devices_at_{};

    // Declared before the captures: their callbacks feed it.
    groovy::AsyncSender mister_;
    std::string address_; // gui_.ip once the user has finished editing it
    std::string connected_ip_;
    bool stream_announced_ = false; // this connection's mode has been read and logged
    groovy::VideoMode active_mode_;
    uint32_t stream_audio_rate_hz_ = 0; // negotiated when the session opened
    clock::time_point next_open_ = clock::now();

    std::unique_ptr<directshow_capture::Capture> capture_;
    std::unique_ptr<directshow_capture::AudioCapture> audio_;
    uint32_t audio_rate_hz_ = 0;

    std::vector<uint8_t> sample_;      // latest raw sample (YUY2, NV12, I420 or JPEG)
    std::vector<uint8_t> planar_yuy2_; // UYVY or 4:2:0 sample repacked for the preview
    std::vector<uint8_t> preview_rgb_; // decoded MJPEG
    unsigned preview_rgb_w_ = 0;
    unsigned preview_rgb_h_ = 0;
    uint64_t preview_rgb_seq_ = 0;
    jpeg_decode::Decoder preview_jpeg_;
    clock::time_point last_sample_at_{};
    bool painting_ = false;
    ImVector<ImWchar> font_ranges_; // read when ImGui builds the atlas

    clock::time_point statistics_start_ = clock::now();
    uint64_t loops_ = 0;
    uint64_t latest_drops_ = 0;
    uint64_t present_skips_ = 0;
    double processing_ms_sum_ = 0.0;
};
