#pragma once

#include <windows.h>

#include "video_view.h"

#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <cwctype>
#include <fstream>
#include <iterator>
#include <string>

// GroovyGrab.ini sits next to the exe. Dropdown values are stored by name and by
// size, not by list index, so a missing card or mode can fall back without
// forgetting the last choice that did exist.

using video_view::kMaxCropPercent;

struct AppSettings {
    std::wstring device;
    bool has_device = false;
    std::wstring audio_device; // empty = match the video device, "none" = no audio
    int width = 0;
    int height = 0;
    double fps = 0.0;
    std::string pixel_format = "yuy2"; // yuy2, uyvy, mjpeg, nv12 or i420
    bool has_format = false;
    std::string address;
    bool progressive = false;
    bool letterbox = false;
    bool show_stats = false;
    int crop_h = 0;
    int crop_v = 0;
    int window_width = 0; // 0 = default
    int window_height = 0;

    bool operator==(const AppSettings&) const = default;
};

inline std::wstring settings_path() {
    wchar_t path[MAX_PATH]{};
    const DWORD length = GetModuleFileNameW(nullptr, path, MAX_PATH);
    if (length == 0 || length >= MAX_PATH) return L"GroovyGrab.ini";
    std::wstring file(path, length);
    return file.substr(0, file.find_last_of(L"\\/") + 1) + L"GroovyGrab.ini";
}

inline std::wstring ini_string(const wchar_t* key, const std::wstring& path) {
    wchar_t value[512]{};
    GetPrivateProfileStringW(L"GroovyGrab", key, L"", value, static_cast<DWORD>(std::size(value)),
                             path.c_str());
    return value;
}

inline int ini_int(const wchar_t* key, int fallback, const std::wstring& path) {
    return static_cast<int>(GetPrivateProfileIntW(L"GroovyGrab", key, fallback, path.c_str()));
}

inline AppSettings load_settings(const std::wstring& path) {
    AppSettings settings;
    settings.device = ini_string(L"device", path);
    settings.has_device = !settings.device.empty();
    settings.audio_device = ini_string(L"audio_device", path);

    settings.width = ini_int(L"width", 0, path);
    settings.height = ini_int(L"height", 0, path);
    settings.fps = _wtof(ini_string(L"fps", path).c_str());
    const std::wstring pixel_format = ini_string(L"pixel_format", path);
    if (!pixel_format.empty()) {
        settings.pixel_format.clear();
        for (const wchar_t c : pixel_format) settings.pixel_format += static_cast<char>(towlower(c));
    }
    settings.has_format = settings.width > 0 && settings.height > 0 && settings.fps > 0.0;

    const std::wstring address = ini_string(L"address", path);
    if (!address.empty()) {
        char utf8[64]{};
        WideCharToMultiByte(CP_UTF8, 0, address.c_str(), -1, utf8, sizeof(utf8), nullptr, nullptr);
        if (utf8[0]) settings.address = utf8;
    }

    settings.window_width = ini_int(L"window_width", 0, path);
    settings.window_height = ini_int(L"window_height", 0, path);
    settings.progressive = ini_int(L"progressive", 0, path) != 0;
    settings.letterbox = ini_int(L"letterbox", 0, path) != 0;
    settings.show_stats = ini_int(L"show_stats", 0, path) != 0;
    settings.crop_h = std::clamp(ini_int(L"crop_h", 0, path), -kMaxCropPercent, kMaxCropPercent);
    settings.crop_v = std::clamp(ini_int(L"crop_v", 0, path), -kMaxCropPercent, kMaxCropPercent);
    return settings;
}

// Writes the whole file in UTF-16, which keeps device names in any alphabet, and swaps
// it in only when complete. False if that failed, as in a read-only folder.
inline bool save_settings(const std::wstring& path, const AppSettings& settings) {
    std::wstring text = L"\xFEFF[GroovyGrab]\r\n";
    const auto add = [&](const wchar_t* key, const std::wstring& value) {
        text += key;
        text += L'=';
        text += value;
        text += L"\r\n";
    };
    if (settings.has_device) add(L"device", settings.device);
    add(L"audio_device", settings.audio_device);
    if (settings.has_format) {
        add(L"width", std::to_wstring(settings.width));
        add(L"height", std::to_wstring(settings.height));
        wchar_t fps[32]{};
        std::swprintf(fps, std::size(fps), L"%.3f", settings.fps);
        add(L"fps", fps);
        add(L"pixel_format", std::wstring(settings.pixel_format.begin(), settings.pixel_format.end()));
    }
    wchar_t address[64]{};
    MultiByteToWideChar(CP_UTF8, 0, settings.address.c_str(), -1, address,
                        static_cast<int>(std::size(address)));
    add(L"address", address);
    add(L"progressive", settings.progressive ? L"1" : L"0");
    add(L"letterbox", settings.letterbox ? L"1" : L"0");
    add(L"show_stats", settings.show_stats ? L"1" : L"0");
    add(L"crop_h", std::to_wstring(settings.crop_h));
    add(L"crop_v", std::to_wstring(settings.crop_v));
    add(L"window_width", std::to_wstring(settings.window_width));
    add(L"window_height", std::to_wstring(settings.window_height));

    const std::wstring temp = path + L".tmp";
    std::ofstream out(temp, std::ios::binary | std::ios::trunc);
    out.write(reinterpret_cast<const char*>(text.data()), static_cast<std::streamsize>(text.size() * sizeof(wchar_t)));
    out.close();
    if (out.good() && MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING)) return true;
    DeleteFileW(temp.c_str());
    return false;
}
