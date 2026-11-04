#include "format_menu.h"

#include "desktop_capture.h"
#include "video_mode.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>

namespace {

using directshow_capture::VideoFormat;
using PixelFormat = VideoFormat::PixelFormat;

constexpr double kSameRate = 0.15;

std::string to_utf8(const std::wstring& value) {
    if (value.empty()) return {};
    const int bytes = WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, nullptr, 0, nullptr, nullptr);
    std::string out(static_cast<size_t>(std::max(0, bytes - 1)), '\0');
    if (bytes > 1) {
        WideCharToMultiByte(CP_UTF8, 0, value.c_str(), -1, out.data(), bytes, nullptr, nullptr);
    }
    return out;
}

std::string rate_text(double fps) {
    char text[32];
    if (std::abs(fps - std::round(fps)) < 0.05) {
        std::snprintf(text, sizeof(text), "%.0f fps", std::round(fps));
    } else {
        std::snprintf(text, sizeof(text), "%.2f fps", fps);
    }
    return text;
}

int closest_rate(const std::vector<double>& rates, double fps) {
    int best = 0;
    double delta = 1e9;
    for (int i = 0; i < static_cast<int>(rates.size()); ++i) {
        const double d = std::abs(rates[static_cast<size_t>(i)] - fps);
        if (d < delta) {
            delta = d;
            best = i;
        }
    }
    return best;
}

template <typename T>
int index_of(const std::vector<T>& items, const T& item) {
    const auto found = std::find(items.begin(), items.end(), item);
    return found == items.end() ? 0 : static_cast<int>(found - items.begin());
}

int clamp_index(int index, size_t count) {
    return count ? std::clamp(index, 0, static_cast<int>(count) - 1) : 0;
}

// What a dropdown shows: identical devices are told apart by their order.
std::vector<std::string> display_names(const std::vector<directshow_capture::Device>& devices) {
    std::vector<std::string> names;
    for (size_t i = 0; i < devices.size(); ++i) {
        const auto same_name = [&](const directshow_capture::Device& other) { return other.name == devices[i].name; };
        const auto earlier = std::count_if(devices.begin(), devices.begin() + static_cast<ptrdiff_t>(i), same_name);
        const bool shared = std::count_if(devices.begin(), devices.end(), same_name) > 1;
        names.push_back(to_utf8(shared ? devices[i].name + L" (" + std::to_wstring(earlier + 1) + L")" : devices[i].name));
    }
    return names;
}

} // namespace

void FormatMenu::refresh_devices(bool allow_substring) {
    const bool listed = gui_.device_index >= 0 && gui_.device_index < static_cast<int>(devices_.size());
    const std::wstring keep = listed ? devices_[static_cast<size_t>(gui_.device_index)].id : device;
    // A card's id changes when an identical one comes or goes; its path does not.
    const std::wstring keep_path = listed ? devices_[static_cast<size_t>(gui_.device_index)].path : L"";
    devices_ = directshow_capture::list_video_devices();
    const auto monitors = desktop_capture::list_monitors();
    devices_.insert(devices_.end(), monitors.begin(), monitors.end());
    gui_.devices = display_names(devices_);
    int exact = -1;
    int partial = -1;
    int first_real = -1;
    for (int i = 0; i < static_cast<int>(devices_.size()); ++i) {
        const auto& candidate = devices_[static_cast<size_t>(i)];
        const std::wstring& name = candidate.name;
        // A plain name saved before a second identical card was added means the first.
        if (!keep_path.empty() ? candidate.path == keep_path
                               : !keep.empty() && (candidate.id == keep || candidate.path == keep || (name == keep && exact < 0))) exact = i;
        else if (allow_substring && !keep.empty() && directshow_capture::contains_insensitive(name, keep)) partial = i;
        // With nothing saved, prefer real hardware over software cameras.
        if (first_real < 0 && !directshow_capture::contains_insensitive(name, L"virtual")) first_real = i;
    }
    gui_.device_index = exact >= 0 ? exact : partial >= 0 ? partial : std::max(first_real, 0);
    device_in_list_ = exact >= 0;
    if (devices_.empty()) {
        formats_.clear();
        gui_.pixel_formats.clear();
        gui_.sizes.clear();
        gui_.rates.clear();
    }

    audio_devices_ = directshow_capture::list_audio_devices();
    gui_.audio_devices = display_names(audio_devices_);
    gui_.audio_devices.insert(gui_.audio_devices.begin(), "None");
}

void FormatMenu::select_audio(const std::wstring& setting) {
    audio_setting_ = setting;
    int index = -1;
    if (setting == L"none") {
        audio_.clear();
        gui_.audio_index = 0;
        return;
    }
    for (int i = 0; i < static_cast<int>(audio_devices_.size()) && index < 0; ++i) {
        const auto& candidate = audio_devices_[static_cast<size_t>(i)];
        if (!setting.empty() && (candidate.id == setting || candidate.name == setting)) index = i;
    }
    if (index < 0) index = audio_of_video_device();
    audio_ = index >= 0 ? audio_devices_[static_cast<size_t>(index)].id : std::wstring();
    gui_.audio_index = index + 1; // item 0 is None
}

void FormatMenu::apply_gui_audio() {
    const int index = gui_.audio_index - 1;
    if (index >= 0 && index < static_cast<int>(audio_devices_.size())) {
        audio_ = audio_devices_[static_cast<size_t>(index)].id;
        audio_setting_ = audio_;
    } else {
        audio_.clear();
        audio_setting_ = L"none";
    }
}

void FormatMenu::use_selected_device() {
    if (gui_.device_index >= 0 && gui_.device_index < static_cast<int>(devices_.size())) {
        device = devices_[static_cast<size_t>(gui_.device_index)].id;
    }
}

bool FormatMenu::select_saved_device(const std::wstring& id) {
    const auto matches = [&](const directshow_capture::Device& candidate) {
        return candidate.id == id || candidate.path == id || candidate.name == id;
    };
    const int count = static_cast<int>(devices_.size());
    if (gui_.device_index >= 0 && gui_.device_index < count && matches(devices_[static_cast<size_t>(gui_.device_index)])) {
        return false;
    }
    const auto found = std::find_if(devices_.begin(), devices_.end(), matches);
    if (found == devices_.end()) return false;
    gui_.device_index = static_cast<int>(found - devices_.begin());
    device_in_list_ = true;
    return true;
}

// The audio device on the same hardware as the active video device, or -1.
int FormatMenu::audio_of_video_device() const {
    for (const auto& video : devices_) {
        if (video.id != device || video.container.empty()) continue;
        for (int i = 0; i < static_cast<int>(audio_devices_.size()); ++i) {
            if (audio_devices_[static_cast<size_t>(i)].container == video.container) return i;
        }
    }
    return -1;
}

void FormatMenu::load_formats(bool keep_format) {
    if (desktop_capture::is_monitor(device)) {
        formats_.clear();
        const auto format = desktop_capture::format_of(device);
        device_default_ = {};
        if (format.width >= 2) {
            for (double fps : {50.0, 60.0}) {
                auto rate = format;
                rate.fps = fps;
                rate.frame_time = fps == 50.0 ? 200000 : 10'000'000 / 60;
                if (fps == 60.0) device_default_ = rate;
                formats_.push_back(std::move(rate));
            }
        }
        if (!keep_format || formats_.empty()) selected = device_default_;
        rebuild(true);
        use_listed_format();
        return;
    }
    formats_ = directshow_capture::list_video_formats(device);
    device_default_ = directshow_capture::default_video_format(device);
    const bool offered = std::any_of(formats_.begin(), formats_.end(), [&](const VideoFormat& format) {
        return format.width == selected.width && format.height == selected.height &&
               format.pixel_format == selected.pixel_format;
    });
    if (!keep_format || (!formats_.empty() && !offered)) selected = device_default_;
    rebuild(true);
    use_listed_format();
}

// A listed format also carries the frame interval, which a saved one does not.
void FormatMenu::use_listed_format() {
    for (const auto& format : formats_) {
        if (format.width == selected.width && format.height == selected.height &&
            format.pixel_format == selected.pixel_format &&
            std::abs(format.fps - selected.fps) < kSameRate) {
            selected = format;
            return;
        }
    }
}

void FormatMenu::apply_gui_choice() {
    rebuild(false);
    use_listed_format();
}

void FormatMenu::set_active_format(const VideoFormat& format) {
    selected = format;
    rebuild(true);
}

// Rebuilds the three format dropdowns, each narrowed by the one before it.
// from_selected: indices follow `selected`; otherwise `selected` follows the
// indices the user changed.
void FormatMenu::rebuild(bool from_selected) {
    gui_.pixel_formats.clear();
    gui_.sizes.clear();
    gui_.rates.clear();

    std::vector<PixelFormat> pixel_keys;
    for (const auto& format : formats_) {
        if (std::find(pixel_keys.begin(), pixel_keys.end(), format.pixel_format) == pixel_keys.end()) {
            pixel_keys.push_back(format.pixel_format);
            gui_.pixel_formats.emplace_back(directshow_capture::pixel_format_name(format.pixel_format));
        }
    }
    gui_.pixel_format_index = from_selected ? index_of(pixel_keys, selected.pixel_format)
                                            : clamp_index(gui_.pixel_format_index, pixel_keys.size());
    const PixelFormat pixel =
        pixel_keys.empty() ? PixelFormat::yuy2 : pixel_keys[static_cast<size_t>(gui_.pixel_format_index)];
    const bool pixel_changed = selected.pixel_format != pixel;
    selected.pixel_format = pixel;

    using Size = std::pair<int, int>;
    const Size default_size{device_default_.width, device_default_.height};
    const bool default_pixel = pixel == device_default_.pixel_format;
    gui_.default_size_index = gui_.default_rate_index = -1;
    std::vector<Size> size_keys;
    for (const auto& format : formats_) {
        const Size key{format.width, format.height};
        if (format.pixel_format != pixel ||
            std::find(size_keys.begin(), size_keys.end(), key) != size_keys.end()) {
            continue;
        }
        if (!desktop_capture::is_monitor(device) && key == default_size && default_pixel) {
            gui_.default_size_index = static_cast<int>(size_keys.size());
        }
        size_keys.push_back(key);
        char text[64];
        std::snprintf(text, sizeof(text), "%d x %d", key.first, key.second);
        gui_.sizes.emplace_back(text);
    }
    const Size selected_size{selected.width, selected.height};
    gui_.size_index = from_selected ? index_of(size_keys, selected_size)
                                    : clamp_index(gui_.size_index, size_keys.size());
    const Size size = size_keys.empty() ? selected_size : size_keys[static_cast<size_t>(gui_.size_index)];
    const bool size_changed = pixel_changed || size != selected_size;

    std::vector<double> rate_keys;
    for (const auto& format : formats_) {
        if (format.pixel_format != pixel || Size{format.width, format.height} != size) continue;
        const bool seen = std::any_of(rate_keys.begin(), rate_keys.end(), [&](double fps) {
            return std::abs(fps - format.fps) < kSameRate;
        });
        if (seen) continue;
        if (!desktop_capture::is_monitor(device) && size == default_size && default_pixel &&
            std::abs(format.fps - device_default_.fps) < kSameRate) {
            gui_.default_rate_index = static_cast<int>(rate_keys.size());
        }
        rate_keys.push_back(format.fps);
        gui_.rates.push_back(rate_text(format.fps));
    }
    gui_.rate_index = from_selected || size_changed ? closest_rate(rate_keys, selected.fps)
                                                    : clamp_index(gui_.rate_index, rate_keys.size());
    selected.width = size.first;
    selected.height = size.second;
    if (!rate_keys.empty()) selected.fps = rate_keys[static_cast<size_t>(gui_.rate_index)];

    gui_.pal = groovy::is_pal_fps(selected.fps);
}
