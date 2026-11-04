#include "app.h"

#include "app_window.h"
#include "imgui_impl_dx11.h"
#include "imgui_impl_win32.h"
#include "imgui_internal.h"
#include "video_mode.h"

#include <algorithm>
#include <cstdio>
#include <exception>

namespace {

using PixelFormat = directshow_capture::VideoFormat::PixelFormat;

// How long after the last sample the panel still shows "Receiving Video".
constexpr auto kVideoTimeout = std::chrono::milliseconds(250);

using directshow_capture::pixel_format_from_setting;
using directshow_capture::pixel_format_setting;

// The preview only draws YUY2 or RGB: luma is copied and each chroma row is
// shared by two lines. NV12 has interleaved UV rows of w bytes; I420 has a U
// plane and a V plane with rows of w/2 bytes.
void repack_420_to_yuy2(const uint8_t* sample, size_t w, size_t h, bool i420,
                        std::vector<uint8_t>& out) {
    out.resize(w * h * 2u);
    const uint8_t* chroma = sample + w * h;
    const size_t chroma_pitch = i420 ? w / 2u : w;
    const size_t step = i420 ? 1 : 2;
    for (size_t y = 0; y < h; ++y) {
        const uint8_t* luma = sample + y * w;
        const uint8_t* u = chroma + (y / 2u) * chroma_pitch;
        const uint8_t* v = i420 ? u + (h / 2u) * chroma_pitch : u + 1;
        uint8_t* row = out.data() + y * w * 2u;
        for (size_t x = 0; x + 1 < w; x += 2) {
            const size_t c = (x / 2) * step;
            row[x * 2 + 0] = luma[x];
            row[x * 2 + 1] = u[c];
            row[x * 2 + 2] = luma[x + 1];
            row[x * 2 + 3] = v[c];
        }
    }
}

// Where Windows is installed, or empty. Checked because ImGui treats a missing font file
// as a programming error.
std::string system_font_path(const wchar_t* file) {
    wchar_t folder[MAX_PATH]{};
    const UINT length = GetWindowsDirectoryW(folder, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return {};
    const std::wstring path = std::wstring(folder) + L"\\Fonts\\" + file;
    if (GetFileAttributesW(path.c_str()) == INVALID_FILE_ATTRIBUTES) return {};
    char utf8[MAX_PATH * 4]{};
    if (WideCharToMultiByte(CP_UTF8, 0, path.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr) == 0) return {};
    return utf8;
}

// Segoe UI lacks these alphabets.
constexpr ImWchar kFirstEastAsian = 0x2E80;

void add_east_asian_chars(const std::vector<std::string>& names, ImFontGlyphRangesBuilder& builder, bool& any) {
    for (const auto& name : names) {
        const char* text = name.c_str();
        while (*text) {
            unsigned int c = 0;
            text += ImTextCharFromUtf8(&c, text, nullptr);
            if (c >= kFirstEastAsian && c <= 0xFFFF) {
                builder.AddChar(static_cast<ImWchar>(c));
                any = true;
            }
        }
    }
}

// The value to save for a control the command line may have set: that only counts
// once the user changes it.
bool value_to_save(std::optional<bool>& override_value, bool shown, bool saved) {
    if (override_value && *override_value != shown) override_value.reset();
    return override_value ? saved : shown;
}

} // namespace

App::App(HWND hwnd, app_gui::Frame& gui, const CommandLine& command_line, const AppSettings& saved)
    : hwnd_(hwnd),
      gui_(gui),
      print_stats_(command_line.print_stats),
      renderer_(hwnd),
      menu_(gui),
      ini_path_(settings_path()),
      settings_(saved),
      written_(saved),
      device_forced_(command_line.device.has_value()),
      progressive_override_(command_line.progressive),
      active_mode_(groovy::VideoMode::i480) {
    if (command_line.letterbox) letterbox_override_ = true;
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    app_gui::apply_theme(gui_.scale);
    ImGui::GetIO().IniFilename = nullptr;
    ImGui_ImplWin32_Init(hwnd);
    ImGui_ImplDX11_Init(renderer_.device.Get(), renderer_.context.Get());
    app_window::set_imgui_ready(true);

    mister_.set_timing_log(command_line.print_stats);
    mister_.set_rate_lock(command_line.rate_lock);

    gui_.progressive = command_line.progressive.value_or(saved.progressive);
    gui_.letterbox = command_line.letterbox || saved.letterbox;
    gui_.show_stats = saved.show_stats;
    gui_.stream = command_line.send;
    gui_.crop_h = saved.crop_h;
    gui_.crop_v = saved.crop_v;
    if (!saved.address.empty() && saved.address.size() < sizeof(gui_.ip)) {
        std::snprintf(gui_.ip, sizeof(gui_.ip), "%s", saved.address.c_str());
    }
    address_ = gui_.ip;

    menu_.device = command_line.device.value_or(saved.device);
    menu_.refresh_devices(!saved.has_device || command_line.device.has_value());
    menu_.use_selected_device();
    menu_.select_audio(saved.audio_device);
    rebuild_fonts();
    if (menu_.has_devices()) {
        use_saved_format();
        menu_.load_formats(saved.has_format);
    }
    open_capture();

    app_window::set_paint_callback([this] {
        if (painting_) return;
        poll_capture(0);
        draw_frame(false);
    });
    // Sleep or shutdown would otherwise leave the last frame on the CRT indefinitely.
    app_window::set_power_down_callback([this] {
        std::printf("Windows is going down: stopping the stream\n");
        gui_.stream = false;
        mister_.close();
    });
}

App::~App() {
    app_window::set_paint_callback(nullptr);
    app_window::set_power_down_callback(nullptr);
    app_window::set_imgui_ready(false);
    ImGui_ImplDX11_Shutdown();
    ImGui_ImplWin32_Shutdown();
    ImGui::DestroyContext();
}

void App::run() {
    while (pump_messages()) {
        poll_capture(app_window::sizing() ? 0 : 30);
        const auto started = clock::now();
        manage_stream();
        draw_frame(true);
        handle_gui_requests();
        processing_ms_sum_ += std::chrono::duration<double, std::milli>(clock::now() - started).count();
        ++loops_;
        update_statistics();
        save_settings_if_changed();
    }
}

bool App::pump_messages() {
    MSG message{};
    while (PeekMessage(&message, nullptr, 0, 0, PM_REMOVE)) {
        if (message.message == WM_QUIT) return false;
        TranslateMessage(&message);
        DispatchMessage(&message);
    }
    return true;
}

// Segoe UI at the display's scale or, without it, ImGui's built-in font. Device names in
// the alphabets it lacks come from the fonts Windows has for them, with only the
// characters in use, as all of them would make a huge atlas. Only between frames.
void App::rebuild_fonts() {
    ImFontGlyphRangesBuilder builder;
    bool east_asian = false;
    add_east_asian_chars(gui_.devices, builder, east_asian);
    add_east_asian_chars(gui_.audio_devices, builder, east_asian);
    font_ranges_.clear();
    builder.BuildRanges(&font_ranges_);

    ImGuiIO& io = ImGui::GetIO();
    io.Fonts->Clear();
    const float size = 16.0f * gui_.scale;
    const std::string segoe = system_font_path(L"segoeui.ttf");
    if (segoe.empty()) {
        ImFontConfig config;
        config.SizePixels = 13.0f * gui_.scale;
        io.Fonts->AddFontDefault(&config);
    } else {
        // Latin with its extensions, Greek, Cyrillic and general punctuation, for device names.
        static const ImWchar ranges[] = {0x0020, 0x024F, 0x0370, 0x03FF, 0x0400, 0x052F, 0x2000, 0x206F, 0};
        io.Fonts->AddFontFromFileTTF(segoe.c_str(), size, nullptr, ranges);
    }
    if (east_asian) {
        for (const wchar_t* file : {L"msyh.ttc", L"malgun.ttf", L"meiryo.ttc"}) {
            const std::string path = system_font_path(file);
            if (path.empty()) continue;
            ImFontConfig config;
            config.MergeMode = true;
            io.Fonts->AddFontFromFileTTF(path.c_str(), size, &config, font_ranges_.Data);
        }
    }
    ImGui_ImplDX11_InvalidateDeviceObjects();
}

// Re-lists the devices. A device that is in use and still there is left alone, so
// this never disturbs a running stream unless that device is gone: then the stream
// stops and the first available device takes over. A saved device that has returned
// takes over again once no stream is running.
void App::update_devices() {
    const bool had_capture = capture_ != nullptr || desktop_ != nullptr;
    const std::wstring previous_audio = menu_.audio();
    const std::vector<std::string> previous_names = gui_.devices;
    const std::vector<std::string> previous_audio_names = gui_.audio_devices;
    menu_.refresh_devices(false);
    if (gui_.devices != previous_names || gui_.audio_devices != previous_audio_names) rebuild_fonts();
    const bool saved_returned = !device_forced_ && settings_.has_device && !gui_.stream &&
                                menu_.select_saved_device(settings_.device);
    if (!saved_returned && had_capture && menu_.device_in_list()) {
        menu_.use_selected_device();
        menu_.select_audio(menu_.audio_setting());
        if (menu_.audio() != previous_audio) open_audio();
        return;
    }
    if (!saved_returned && had_capture) {
        std::printf("Capture device lost\n");
        gui_.stream = false;
        mister_.close();
    }
    close_capture();
    menu_.use_selected_device();
    menu_.select_audio(menu_.audio_setting());
    if (menu_.has_devices()) {
        if (saved_returned) use_saved_format();
        menu_.load_formats(true);
    }
    open_capture();
}

void App::use_saved_format() {
    if (!settings_.has_format) return;
    menu_.selected.width = settings_.width;
    menu_.selected.height = settings_.height;
    menu_.selected.fps = settings_.fps;
    menu_.selected.pixel_format = pixel_format_from_setting(settings_.pixel_format);
}

void App::open_capture() {
    close_capture();
    mister_.reset_handoff();
    mister_.restart_rate_lock();
    gui_.stats.capture_error.clear();
    if (!menu_.has_devices()) {
        gui_.stats.capture_error = "No capture device found";
        return;
    }
    try {
        if (desktop_capture::is_monitor(menu_.device)) {
            desktop_capture::TickHooks hooks;
            hooks.start = [this](const uint8_t* data, long pitch, unsigned width, unsigned height) {
                return mister_.submit_bgra_held(data, pitch, width, height);
            };
            hooks.tick = [this] { mister_.note_desktop_tick(); };
            hooks.release = [this] { return mister_.release_held_frame(); };
            hooks.budget = [this] { return mister_.scale_budget(); };
            hooks.busy = [this] { return mister_.desktop_scale_busy(); };
            hooks.scale_done = mister_.held_scale_event();
            desktop_ = std::make_unique<desktop_capture::DesktopCapture>(
                menu_.device, std::move(hooks), menu_.selected.fps);
            menu_.set_active_format(desktop_->format());
        } else {
            capture_ = std::make_unique<directshow_capture::Capture>(
                menu_.device, menu_.selected,
                [this](const uint8_t* data, long size, long pitch,
                   const directshow_capture::VideoFormat& format, LONGLONG media_100ns) {
                // Runs on the driver's thread, before the preview ever sees the sample.
                const unsigned w = static_cast<unsigned>(format.width);
                const unsigned h = static_cast<unsigned>(format.height);
                if (format.pixel_format == PixelFormat::mjpeg) {
                    mister_.submit_mjpeg(data, static_cast<size_t>(size), w, h, media_100ns);
                } else if (format.pixel_format == PixelFormat::yuy2 ||
                           format.pixel_format == PixelFormat::uyvy) {
                    mister_.submit_packed422(data, pitch, w, h, format.pixel_format == PixelFormat::uyvy,
                                             media_100ns);
                } else {
                    mister_.submit_planar420(data, pitch, w, h, format.pixel_format == PixelFormat::i420,
                                             media_100ns);
                }
            });
            menu_.set_active_format(capture_->format());
        }
    } catch (const std::exception& error) {
        gui_.stats.capture_error = dynamic_cast<const directshow_capture::UnsupportedFormat*>(&error)
                                       ? "This device can't use the selected format"
                                       : "Couldn't open the capture device. Is another app using it?";
        std::printf("Capture open failed: %s\n", error.what());
        return;
    }
    open_audio();
}

void App::open_audio() {
    audio_.reset();
    audio_rate_hz_ = 0;
    gui_.stats.audio_error.clear();
    if (menu_.audio().empty()) return;
    try {
        audio_ = std::make_unique<directshow_capture::AudioCapture>(
            menu_.audio(),
            [this](const uint8_t* pcm, size_t bytes) { mister_.submit_pcm(pcm, bytes); });
        audio_rate_hz_ = audio_->sample_rate();
    } catch (const std::exception& error) {
        gui_.stats.audio_error = dynamic_cast<const directshow_capture::UnsupportedFormat*>(&error)
                                     ? "This audio device's format isn't supported"
                                     : "Couldn't open the audio device";
        std::printf("Audio open failed: %s\n", error.what());
    }
}

void App::close_capture() {
    audio_.reset();
    audio_rate_hz_ = 0;
    gui_.stats.audio_error.clear();
    capture_.reset();
    desktop_.reset();
    renderer_.clear_image();
    last_sample_at_ = {};
}

// Takes the latest sample for the preview and the source statistics. Streaming
// does not depend on this: the capture callback has already submitted it.
bool App::poll_capture(DWORD timeout_ms) {
    if (!capture_ && !desktop_) {
        if (timeout_ms) Sleep(16);
        return false;
    }
    if (desktop_) {
        LONGLONG timestamp = 0;
        if (!desktop_->wait_for_sample(sample_, timestamp, timeout_ms)) return false;
        const auto& format = desktop_->format();
        renderer_.upload_bgra(sample_.data(), format.width * 4, static_cast<UINT>(format.width),
                              static_cast<UINT>(format.height));
        latest_drops_ += desktop_->take_replaced_samples();
        last_sample_at_ = clock::now();
        return true;
    }
    LONGLONG timestamp = 0;
    const bool got_sample = capture_->wait_for_sample(sample_, timestamp, timeout_ms);
    const auto& format = capture_->format();
    if (format.pixel_format == PixelFormat::mjpeg) refresh_mjpeg_preview(got_sample);
    if (!got_sample) return false;

    if (format.pixel_format == PixelFormat::yuy2) {
        renderer_.upload_yuy2(sample_.data(), format.width * 2, format.width, format.height);
    } else if (format.pixel_format == PixelFormat::uyvy) {
        // Same samples as YUY2 with each byte pair swapped.
        planar_yuy2_.resize(sample_.size());
        for (size_t i = 0; i + 1 < sample_.size(); i += 2) {
            planar_yuy2_[i] = sample_[i + 1];
            planar_yuy2_[i + 1] = sample_[i];
        }
        renderer_.upload_yuy2(planar_yuy2_.data(), format.width * 2, format.width, format.height);
    } else if (format.pixel_format != PixelFormat::mjpeg) {
        upload_planar_preview();
    }

    latest_drops_ += capture_->take_replaced_samples();
    last_sample_at_ = clock::now();
    return true;
}

// While streaming, reuses the decode the prep thread already did.
void App::refresh_mjpeg_preview(bool new_sample) {
    bool fresh = false;
    if (mister_.connected()) {
        fresh = mister_.copy_preview(preview_rgb_, preview_rgb_w_, preview_rgb_h_, preview_rgb_seq_);
    } else if (new_sample) {
        const auto& format = capture_->format();
        std::string error;
        fresh = preview_jpeg_.decompress(sample_.data(), sample_.size(), format.width, format.height,
                                         preview_rgb_, &error);
        preview_rgb_w_ = static_cast<unsigned>(format.width);
        preview_rgb_h_ = static_cast<unsigned>(format.height);
    }
    if (fresh) {
        renderer_.upload_rgb(preview_rgb_.data(), static_cast<LONG>(preview_rgb_w_) * 3,
                             preview_rgb_w_, preview_rgb_h_);
    }
}

void App::upload_planar_preview() {
    const auto& format = capture_->format();
    const size_t w = static_cast<size_t>(format.width);
    const size_t h = static_cast<size_t>(format.height);
    repack_420_to_yuy2(sample_.data(), w, h, format.pixel_format == PixelFormat::i420, planar_yuy2_);
    renderer_.upload_yuy2(planar_yuy2_.data(), format.width * 2, format.width, format.height);
}

groovy::VideoMode App::wanted_mode() const {
    return groovy::video_mode_for(gui_.pal, gui_.progressive);
}

void App::manage_stream() {
    if (gui_.ip_changed) {
        gui_.ip_changed = false;
        address_ = gui_.ip;
        if ((mister_.connected() || mister_.opening()) && connected_ip_ != address_) {
            mister_.close();
        }
        next_open_ = clock::now();
    }
    // Nothing to stream: a capture that failed to open or could not be reopened.
    if (!capture_ && !desktop_) gui_.stream = false;
    if (gui_.stream && !mister_.connected()) {
        stream_announced_ = false;
        const auto now = clock::now();
        if (!mister_.opening() && now >= next_open_) {
            next_open_ = now + std::chrono::seconds(2);
            active_mode_ = wanted_mode();
            connected_ip_ = address_;
            stream_audio_rate_hz_ = audio_rate_hz_;
            mister_.start_open(connected_ip_, active_mode_, audio_rate_hz_);
        }
    } else if (!gui_.stream && (mister_.connected() || mister_.opening())) {
        mister_.close();
        stream_announced_ = false;
    } else if (mister_.connected() && !stream_announced_) {
        stream_announced_ = true;
        active_mode_ = mister_.mode();
        gui_.pal = groovy::is_pal_mode(active_mode_);
        std::printf("MiSTer connected %s mode %s audio %u\n", connected_ip_.c_str(),
                    groovy::video_mode_name(active_mode_), audio_rate_hz_);
    }

    if (mister_.connected() && mister_.recalibrate_needed()) {
        mister_.start_recalibrate();
        return;
    }

    if (mister_.connected() && stream_audio_rate_hz_ != audio_rate_hz_) {
        std::printf("MiSTer audio %u -> %u Hz\n", stream_audio_rate_hz_, audio_rate_hz_);
        stream_audio_rate_hz_ = audio_rate_hz_;
        if (!mister_.set_audio(audio_rate_hz_)) gui_.stream = false;
    }

    if (mister_.connected()) {
        if (active_mode_ != wanted_mode()) {
            active_mode_ = wanted_mode();
            if (mister_.set_mode(active_mode_)) {
                active_mode_ = mister_.mode();
                gui_.pal = groovy::is_pal_mode(active_mode_);
                std::printf("MiSTer switched to %s\n", groovy::video_mode_name(active_mode_));
            } else {
                gui_.stream = false;
            }
        }
        mister_.set_view({gui_.letterbox, gui_.crop_h, gui_.crop_v});
    }
}

void App::draw_frame(bool from_main_loop) {
    painting_ = true;
    ImGui_ImplDX11_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    gui_.stats.mister_connected = mister_.connected();
    gui_.stats.mister_calibrating = mister_.calibrating();
    gui_.stats.mister_error = mister_.error();
    gui_.stats.audio_rate_hz = audio_rate_hz_;
    gui_.stats.allocator_buffers = capture_ ? capture_->buffer_count() : -1;
    gui_.stats.capture_warning = mister_.capture_unusual() ? "Capture rate is free-running" : "";
    gui_.has_capture = capture_ != nullptr || desktop_ != nullptr;
    gui_.has_video = (capture_ || desktop_) && last_sample_at_ != clock::time_point{} &&
                     clock::now() - last_sample_at_ < kVideoTimeout;
    app_gui::draw(gui_);
    renderer_.draw(gui_.video_x, gui_.video_y, gui_.video_w, gui_.video_h, gui_.letterbox,
                   gui_.crop_h, gui_.crop_v);
    ImGui::Render();
    ImGui_ImplDX11_RenderDrawData(ImGui::GetDrawData());
    // The main loop must never wait on the display; a repaint during a modal
    // resize has to reach the screen.
    const bool presented = from_main_loop
                               ? renderer_.present_frame(true, !app_window::sizing())
                               : renderer_.present_frame(false, false);
    if (!presented) ++present_skips_;
    painting_ = false;
}

void App::handle_gui_requests() {
    if (gui_.close) {
        gui_.close = false;
        DestroyWindow(hwnd_);
    }
    if (gui_.minimize) {
        gui_.minimize = false;
        ShowWindow(hwnd_, SW_MINIMIZE);
    }
    if (gui_.toggle_maximize) {
        gui_.toggle_maximize = false;
        WINDOWPLACEMENT placement{sizeof(placement)};
        GetWindowPlacement(hwnd_, &placement);
        ShowWindow(hwnd_, placement.showCmd == SW_SHOWMAXIMIZED ? SW_RESTORE : SW_MAXIMIZE);
    }
    if (gui_.scale_changed) {
        gui_.scale_changed = false;
        app_gui::apply_theme(gui_.scale);
        rebuild_fonts();
    }
    const auto now = clock::now();
    if (gui_.devices_changed) {
        // Once now, for a device that left, and again when a new one has settled.
        gui_.devices_changed = false;
        gui_.refresh_devices = true;
        recheck_devices_at_ = now + std::chrono::seconds(1);
    } else if (recheck_devices_at_ != clock::time_point{} && now >= recheck_devices_at_) {
        gui_.refresh_devices = true;
        recheck_devices_at_ = {};
    }
    if (gui_.refresh_devices) {
        gui_.refresh_devices = false;
        update_devices();
    }
    if (gui_.device_changed) {
        gui_.device_changed = false;
        gui_.format_changed = false;
        close_capture();
        menu_.use_selected_device();
        menu_.select_audio(L""); // follow the new video device
        menu_.load_formats(true);
        open_capture();
        remember_device();
        remember_format();
    } else if (gui_.format_changed) {
        gui_.format_changed = false;
        close_capture();
        menu_.apply_gui_choice();
        open_capture();
        // Choosing a format for the device in use makes it the one to keep.
        remember_device();
        remember_format();
    }
    if (gui_.audio_changed) {
        gui_.audio_changed = false;
        menu_.apply_gui_audio();
        open_audio();
    }
}

void App::update_statistics() {
    const std::string sender_log = mister_.take_log();
    if (!sender_log.empty()) std::printf("%s", sender_log.c_str());
    const auto now = clock::now();
    const double elapsed = std::chrono::duration<double>(now - statistics_start_).count();
    if (elapsed < 1.0) return;

    const auto sent = mister_.take_statistics();
    auto& stats = gui_.stats;
    const uint64_t samples = capture_ ? capture_->take_samples() : desktop_ ? desktop_->take_samples() : 0;
    const uint64_t source_gaps = capture_ ? capture_->take_gaps() : desktop_ ? desktop_->take_gaps() : 0;
    stats.fps = static_cast<double>(samples) / elapsed;
    stats.app_ms = processing_ms_sum_ / static_cast<double>(std::max<uint64_t>(1, loops_));
    stats.source_gaps = source_gaps;
    stats.latest_drops = latest_drops_;
    stats.present_skips = present_skips_;
    stats.mister_ms = sent.send_ms / static_cast<double>(std::max<uint64_t>(1, sent.sent));
    stats.decode_ms = mister_.take_decode_ms();
    stats.mister_duplicates = sent.duplicates;
    stats.mister_replaced = sent.replaced;
    stats.mister_errors = sent.errors;
    if (print_stats_) {
        std::printf("stats fps %.2f app %.2f ms decode %.2f ms send %.2f ms "
                    "sent %llu replaced %llu errors %llu gaps %llu drops %llu\n",
                    stats.fps, stats.app_ms, stats.decode_ms, stats.mister_ms,
                    static_cast<unsigned long long>(sent.sent),
                    static_cast<unsigned long long>(sent.replaced),
                    static_cast<unsigned long long>(sent.errors),
                    static_cast<unsigned long long>(source_gaps),
                    static_cast<unsigned long long>(latest_drops_));
        const std::string latency = mister_.take_probe_line();
        if (!latency.empty()) std::printf("%s\n", latency.c_str());
    }
    // A card can deliver another rate than it advertises, and PAL or NTSC follows that.
    double measured_fps = 0.0;
    if (!mister_.connected() && !mister_.opening() && mister_.measured_capture_rate(measured_fps) &&
        measured_fps > 20.0 && measured_fps < 130.0) {
        gui_.pal = groovy::is_pal_fps(measured_fps);
    }
    statistics_start_ = now;
    loops_ = 0;
    latest_drops_ = 0;
    present_skips_ = 0;
    processing_ms_sum_ = 0.0;
}

void App::remember_device() {
    settings_.device = menu_.device;
    settings_.has_device = menu_.has_devices() && !menu_.device.empty();
}

void App::remember_format() {
    const auto& selected = menu_.selected;
    settings_.width = selected.width;
    settings_.height = selected.height;
    settings_.fps = selected.fps;
    settings_.pixel_format = pixel_format_setting(selected.pixel_format);
    settings_.has_format = selected.width > 0 && selected.height > 0 && selected.fps > 0.0;
}

void App::save_settings_if_changed() {
    // Waits for the end of a slider drag, which changes the value many times.
    if (ImGui::IsAnyItemActive()) return;
    if (!settings_.has_device) remember_device();
    if (!settings_.has_format) remember_format();
    settings_.audio_device = menu_.audio_setting();
    app_window::normal_size(hwnd_, settings_.window_width, settings_.window_height);
    settings_.address = address_;
    settings_.progressive = value_to_save(progressive_override_, gui_.progressive, settings_.progressive);
    settings_.letterbox = value_to_save(letterbox_override_, gui_.letterbox, settings_.letterbox);
    settings_.show_stats = gui_.show_stats;
    settings_.crop_h = std::clamp(gui_.crop_h, -kMaxCropPercent, kMaxCropPercent);
    settings_.crop_v = std::clamp(gui_.crop_v, -kMaxCropPercent, kMaxCropPercent);
    if (settings_ == written_) return;
    const bool saved = save_settings(ini_path_, settings_);
    if (!saved && gui_.stats.settings_error.empty()) {
        std::wprintf(L"Settings save failed: %ls\n", ini_path_.c_str());
    }
    gui_.stats.settings_error = saved ? "" : "Couldn't save settings. Move GroovyGrab to a folder you can write to";
    written_ = settings_;
}
