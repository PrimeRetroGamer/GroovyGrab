#pragma once

#include "app_gui.h"
#include "directshow_capture.h"

#include <string>
#include <vector>

// Backs the Device, Colour Format, Resolution and Frame Rate dropdowns: what the
// capture devices offer, and which mode is selected.
class FormatMenu {
public:
    using VideoFormat = directshow_capture::VideoFormat;

    explicit FormatMenu(app_gui::Frame& gui) : gui_(gui) {}

    // Re-lists devices, keeping the current one if present. `allow_substring`
    // also accepts a partial name match, for names typed on the command line.
    void refresh_devices(bool allow_substring);

    // Device currently selected in the dropdown became the active one.
    void use_selected_device();

    // Selects the device saved as `id`. False if it's absent or already selected.
    bool select_saved_device(const std::wstring& id);

    // Selects audio from a saved setting: a device, "none", or empty to use the
    // audio of the active video device, if it has any. A saved device that is
    // absent does the same, without forgetting the setting.
    void select_audio(const std::wstring& setting);

    // The user picked from the audio dropdown.
    void apply_gui_audio();

    // The audio device to open; empty for none.
    const std::wstring& audio() const { return audio_; }
    const std::wstring& audio_setting() const { return audio_setting_; }

    // Lists the active device's modes. `keep_format` keeps the current selection
    // if the device offers it, instead of switching to the device's default.
    void load_formats(bool keep_format);

    // The user changed a format dropdown.
    void apply_gui_choice();

    // The device gave us `format`, which may differ from what was asked for.
    void set_active_format(const VideoFormat& format);

    bool has_devices() const { return !devices_.empty(); }
    bool device_in_list() const { return device_in_list_; }

    // The device's name, or its path when another device has the same name.
    std::wstring device;
    VideoFormat selected{};

private:
    void rebuild(bool from_selected);
    void use_listed_format();
    int audio_of_video_device() const;

    app_gui::Frame& gui_;
    std::vector<directshow_capture::Device> devices_;
    std::vector<directshow_capture::Device> audio_devices_;
    std::wstring audio_;
    std::wstring audio_setting_;
    std::vector<VideoFormat> formats_;
    VideoFormat device_default_{};
    bool device_in_list_ = false;
};
