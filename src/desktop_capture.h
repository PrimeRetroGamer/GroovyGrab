#pragma once

#include "directshow_capture.h"

#include <chrono>
#include <cstdint>
#include <functional>
#include <future>
#include <memory>
#include <string>
#include <vector>

// One SDR monitor, read with Desktop Duplication. A picture is scaled when it
// arrives. The selected 50 or 60 Hz tick releases the newest one that finished
// in time, or the previous one when a newer picture is still being scaled.
// HDR outputs are not listed.
namespace desktop_capture {

struct TickHooks {
    // Copy this picture into the scaler. False if it was not accepted.
    std::function<bool(const uint8_t* data, long pitch, unsigned width, unsigned height)> start;
    // One beat of the selected clock, used to measure the capture rate.
    std::function<void()> tick;
    // Hand the finished picture to the sender. False if none is ready yet.
    std::function<bool()> release;
    // How long a newer picture needs before this tick.
    std::function<std::chrono::nanoseconds()> budget;
    // True while a picture started earlier is still scaling.
    std::function<bool()> busy;
    HANDLE scale_done = nullptr;
};

bool is_monitor(const std::wstring& id);
std::vector<directshow_capture::Device> list_monitors();
directshow_capture::VideoFormat format_of(const std::wstring& id);

class DesktopCapture {
public:
    // `fps` is 50 or 60. Anything else runs at 60.
    DesktopCapture(const std::wstring& id, TickHooks hooks, double fps);
    ~DesktopCapture();

    DesktopCapture(const DesktopCapture&) = delete;
    DesktopCapture& operator=(const DesktopCapture&) = delete;

    bool wait_for_sample(std::vector<uint8_t>& dest, LONGLONG& timestamp, DWORD timeout_ms);
    uint64_t take_replaced_samples();
    uint64_t take_samples();
    uint64_t take_gaps();

    const std::wstring& device_name() const { return device_name_; }
    const directshow_capture::VideoFormat& format() const { return format_; }

private:
    struct Runner;
    void run(std::promise<std::string>& opened);
    void show_preview(const std::vector<uint8_t>& shown, unsigned width, unsigned height);

    TickHooks hooks_;
    double fps_ = 60.0;
    std::wstring id_;
    std::wstring device_name_;
    directshow_capture::VideoFormat format_{};
    std::unique_ptr<Runner> runner_;
};

} // namespace desktop_capture
