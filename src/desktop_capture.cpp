#include "desktop_capture.h"

#include <windows.h>
#include <d3d11.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <timeapi.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <future>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <vector>

#ifndef CREATE_WAITABLE_TIMER_HIGH_RESOLUTION
#define CREATE_WAITABLE_TIMER_HIGH_RESOLUTION 0x00000002
#endif

using Microsoft::WRL::ComPtr;

namespace desktop_capture {

namespace {

constexpr wchar_t kPrefix[] = L"monitor:";

double desktop_rate(double fps) {
    return std::abs(fps - 50.0) < 1.0 ? 50.0 : 60.0;
}

bool sdr(DXGI_COLOR_SPACE_TYPE space) {
    switch (space) {
    case DXGI_COLOR_SPACE_RGB_FULL_G22_NONE_P709:
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P709:
    case DXGI_COLOR_SPACE_RGB_STUDIO_G22_NONE_P2020:
        return true;
    default:
        return false;
    }
}

std::wstring monitor_label(const DXGI_OUTPUT_DESC& desc) {
    DISPLAY_DEVICEW device{};
    device.cb = sizeof(device);
    if (EnumDisplayDevicesW(desc.DeviceName, 0, &device, 0) && device.DeviceString[0]) {
        return std::wstring(L"Desktop - ") + device.DeviceString;
    }
    return std::wstring(L"Desktop - ") + desc.DeviceName;
}

void blend_pixel(uint8_t* dest, uint8_t b, uint8_t g, uint8_t r, uint8_t a) {
    dest[0] = static_cast<uint8_t>((b * a + dest[0] * (255 - a)) / 255);
    dest[1] = static_cast<uint8_t>((g * a + dest[1] * (255 - a)) / 255);
    dest[2] = static_cast<uint8_t>((r * a + dest[2] * (255 - a)) / 255);
    dest[3] = 255;
}

void paint_cursor(uint8_t* image, int width, int height, const std::vector<uint8_t>& shape,
                  const DXGI_OUTDUPL_POINTER_SHAPE_INFO& info, int left, int top) {
    if (shape.empty() || info.Width == 0 || info.Height == 0) return;
    const auto at = [&](int x, int y) -> uint8_t* {
        if (x < 0 || y < 0 || x >= width || y >= height) return nullptr;
        return image + (static_cast<size_t>(y) * static_cast<size_t>(width) + static_cast<size_t>(x)) * 4u;
    };
    if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_COLOR ||
        info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
        for (UINT row = 0; row < info.Height; ++row) {
            const uint8_t* src = shape.data() + static_cast<size_t>(row) * info.Pitch;
            if (src + static_cast<size_t>(info.Width) * 4u > shape.data() + shape.size()) return;
            for (UINT col = 0; col < info.Width; ++col) {
                uint8_t* dest = at(left + static_cast<int>(col), top + static_cast<int>(row));
                if (!dest) continue;
                const uint8_t* pixel = src + static_cast<size_t>(col) * 4u;
                if (info.Type == DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MASKED_COLOR) {
                    // The alpha byte is a mask: 0 replaces the screen pixel, 0xFF inverts it.
                    if (pixel[3]) {
                        dest[0] ^= pixel[0];
                        dest[1] ^= pixel[1];
                        dest[2] ^= pixel[2];
                    } else {
                        dest[0] = pixel[0];
                        dest[1] = pixel[1];
                        dest[2] = pixel[2];
                    }
                    dest[3] = 255;
                } else {
                    blend_pixel(dest, pixel[0], pixel[1], pixel[2], pixel[3]);
                }
            }
        }
        return;
    }
    if (info.Type != DXGI_OUTDUPL_POINTER_SHAPE_TYPE_MONOCHROME || info.Pitch == 0) return;
    // One mask is `Height` rows when the buffer holds both masks. Some drivers
    // report the combined height instead, so the buffer size decides.
    const size_t one_mask = static_cast<size_t>(info.Height) * info.Pitch;
    UINT rows = info.Height;
    const uint8_t* xor_base = nullptr;
    if (shape.size() >= one_mask * 2u) {
        xor_base = shape.data() + one_mask;
    } else if (info.Height >= 2 && shape.size() >= one_mask) {
        rows = info.Height / 2u;
        xor_base = shape.data() + static_cast<size_t>(rows) * info.Pitch;
    }
    if (!xor_base) return;
    for (UINT row = 0; row < rows; ++row) {
        const uint8_t* and_row = shape.data() + static_cast<size_t>(row) * info.Pitch;
        const uint8_t* xor_row = xor_base + static_cast<size_t>(row) * info.Pitch;
        if (xor_row + (info.Width + 7u) / 8u > shape.data() + shape.size()) return;
        for (UINT col = 0; col < info.Width; ++col) {
            uint8_t* dest = at(left + static_cast<int>(col), top + static_cast<int>(row));
            if (!dest) continue;
            const UINT byte = col / 8u;
            const uint8_t bit = static_cast<uint8_t>(0x80u >> (col % 8u));
            const bool and_bit = (and_row[byte] & bit) != 0;
            const bool xor_bit = (xor_row[byte] & bit) != 0;
            if (!and_bit && !xor_bit) {
                dest[0] = dest[1] = dest[2] = 0;
            } else if (!and_bit && xor_bit) {
                dest[0] = dest[1] = dest[2] = 255;
            } else if (and_bit && xor_bit) {
                dest[0] ^= 255;
                dest[1] ^= 255;
                dest[2] ^= 255;
            }
            dest[3] = 255;
        }
    }
}

struct Pointer {
    std::vector<uint8_t> shape;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO info{};
    int x = 0;
    int y = 0;
    bool visible = false;
};

void read_pointer(IDXGIOutputDuplication* duplication, const DXGI_OUTDUPL_FRAME_INFO& frame, Pointer& pointer) {
    pointer.visible = frame.PointerPosition.Visible != FALSE;
    pointer.x = frame.PointerPosition.Position.x;
    pointer.y = frame.PointerPosition.Position.y;
    if (!frame.PointerShapeBufferSize) return;
    std::vector<uint8_t> shape(frame.PointerShapeBufferSize);
    UINT needed = 0;
    DXGI_OUTDUPL_POINTER_SHAPE_INFO info{};
    if (FAILED(duplication->GetFramePointerShape(static_cast<UINT>(shape.size()), shape.data(), &needed, &info))) {
        return;
    }
    pointer.shape.swap(shape);
    pointer.info = info;
}

bool copy_desktop(ID3D11Device* device, ID3D11DeviceContext* context, ID3D11Texture2D* source,
                  ComPtr<ID3D11Texture2D>& staging, std::vector<uint8_t>& dest, unsigned& width, unsigned& height) {
    D3D11_TEXTURE2D_DESC desc{};
    source->GetDesc(&desc);
    if (desc.Format != DXGI_FORMAT_B8G8R8A8_UNORM || desc.Width < 2 || desc.Height < 1) return false;
    D3D11_TEXTURE2D_DESC staged{};
    if (staging) staging->GetDesc(&staged);
    if (!staging || staged.Width != desc.Width || staged.Height != desc.Height) {
        staging.Reset();
        D3D11_TEXTURE2D_DESC stage = desc;
        stage.Usage = D3D11_USAGE_STAGING;
        stage.BindFlags = 0;
        stage.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
        stage.MiscFlags = 0;
        stage.MipLevels = 1;
        stage.ArraySize = 1;
        stage.SampleDesc.Count = 1;
        stage.SampleDesc.Quality = 0;
        if (FAILED(device->CreateTexture2D(&stage, nullptr, &staging))) return false;
    }
    context->CopyResource(staging.Get(), source);
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context->Map(staging.Get(), 0, D3D11_MAP_READ, 0, &mapped))) return false;
    width = desc.Width;
    height = desc.Height;
    dest.resize(static_cast<size_t>(width) * height * 4u);
    for (unsigned y = 0; y < height; ++y) {
        std::memcpy(dest.data() + static_cast<size_t>(y) * width * 4u,
                    static_cast<const uint8_t*>(mapped.pData) + static_cast<size_t>(y) * mapped.RowPitch,
                    static_cast<size_t>(width) * 4u);
    }
    context->Unmap(staging.Get(), 0);
    return true;
}

// The tick has to wake on the frame. WaitForSingleObject follows the system
// timer, which is too coarse until a 1 ms period is requested.
struct TickClock {
    HANDLE timer = nullptr;
    bool period = false;

    TickClock() {
        timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                                       TIMER_MODIFY_STATE | SYNCHRONIZE);
        if (!timer) {
            timer = CreateWaitableTimerExW(nullptr, nullptr, 0, TIMER_MODIFY_STATE | SYNCHRONIZE);
        }
        period = timeBeginPeriod(1) == TIMERR_NOERROR;
    }

    ~TickClock() {
        if (timer) CloseHandle(timer);
        if (period) timeEndPeriod(1);
    }

    HANDLE handle() const { return timer; }

    // Arms a one-shot wake at `when`. False when that time has already passed
    // or no timer could be created.
    bool arm(std::chrono::steady_clock::time_point when) {
        const auto left = when - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero() || !timer) return false;
        auto hundred_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(left).count() / 100;
        if (hundred_ns <= 0) return false;
        LARGE_INTEGER due;
        due.QuadPart = -hundred_ns;
        return SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE) != FALSE;
    }

    bool sleep_until(std::chrono::steady_clock::time_point when, HANDLE stop) {
        const auto left = when - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero()) return true;
        auto hundred_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(left).count() / 100;
        if (hundred_ns <= 0) return true;
        if (timer) {
            LARGE_INTEGER due;
            due.QuadPart = -hundred_ns;
            if (SetWaitableTimer(timer, &due, 0, nullptr, nullptr, FALSE)) {
                HANDLE handles[2] = {stop, timer};
                return WaitForMultipleObjects(2, handles, FALSE, INFINITE) != WAIT_OBJECT_0;
            }
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(left);
        if (ms.count() <= 0) return true;
        return WaitForSingleObject(stop, static_cast<DWORD>(ms.count())) != WAIT_OBJECT_0;
    }
};

} // namespace

struct DesktopCapture::Runner {
    std::mutex mutex;
    HANDLE ready = nullptr;
    HANDLE stop = nullptr;
    HANDLE image_ready = nullptr;
    std::vector<uint8_t> latest;
    LONGLONG timestamp = -1;
    bool have = false;
    std::atomic<bool> stopping{false};
    // The monitor is being read. A still desktop sends no new pictures, so this,
    // not their arrival, says whether there is video.
    std::atomic<bool> live{false};
    std::atomic<uint64_t> samples{0};
    std::atomic<uint64_t> replaced{0};
    std::mutex image_mutex;
    std::vector<uint8_t> image;
    unsigned image_w = 0;
    unsigned image_h = 0;
    uint64_t image_serial = 0;
    std::thread thread;

    ~Runner() {
        if (ready) CloseHandle(ready);
        if (stop) CloseHandle(stop);
        if (image_ready) CloseHandle(image_ready);
    }
};

bool is_monitor(const std::wstring& id) {
    return id.rfind(kPrefix, 0) == 0;
}

std::vector<directshow_capture::Device> list_monitors() {
    std::vector<directshow_capture::Device> monitors;
    ComPtr<IDXGIFactory1> factory;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return monitors;
    for (UINT adapter_index = 0;; ++adapter_index) {
        ComPtr<IDXGIAdapter1> adapter;
        if (factory->EnumAdapters1(adapter_index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
        for (UINT output_index = 0;; ++output_index) {
            ComPtr<IDXGIOutput> output;
            if (adapter->EnumOutputs(output_index, &output) == DXGI_ERROR_NOT_FOUND) break;
            ComPtr<IDXGIOutput6> output6;
            if (SUCCEEDED(output.As(&output6))) {
                DXGI_OUTPUT_DESC1 desc1{};
                if (SUCCEEDED(output6->GetDesc1(&desc1)) && !sdr(desc1.ColorSpace)) continue;
            }
            DXGI_OUTPUT_DESC desc{};
            if (FAILED(output->GetDesc(&desc)) || desc.AttachedToDesktop == FALSE) continue;
            const int width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
            const int height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
            if (width < 2 || height < 1) continue;
            directshow_capture::Device device;
            device.name = monitor_label(desc);
            device.id = std::wstring(kPrefix) + desc.DeviceName;
            device.path = device.id;
            monitors.push_back(std::move(device));
        }
    }
    return monitors;
}

directshow_capture::VideoFormat format_of(const std::wstring& id) {
    directshow_capture::VideoFormat format;
    for (const auto& monitor : list_monitors()) {
        if (monitor.id != id) continue;
        ComPtr<IDXGIFactory1> factory;
        if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) return format;
        for (UINT adapter_index = 0;; ++adapter_index) {
            ComPtr<IDXGIAdapter1> adapter;
            if (factory->EnumAdapters1(adapter_index, &adapter) == DXGI_ERROR_NOT_FOUND) break;
            for (UINT output_index = 0;; ++output_index) {
                ComPtr<IDXGIOutput> output;
                if (adapter->EnumOutputs(output_index, &output) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_OUTPUT_DESC desc{};
                if (FAILED(output->GetDesc(&desc))) continue;
                if (std::wstring(kPrefix) + desc.DeviceName != id) continue;
                format.width = desc.DesktopCoordinates.right - desc.DesktopCoordinates.left;
                format.height = desc.DesktopCoordinates.bottom - desc.DesktopCoordinates.top;
                format.fps = 60.0;
                format.frame_time = 10'000'000 / 60;
                format.pixel_format = directshow_capture::VideoFormat::PixelFormat::bgra;
                return format;
            }
        }
    }
    return format;
}

DesktopCapture::DesktopCapture(const std::wstring& id, TickHooks hooks, double fps)
    : hooks_(std::move(hooks)), fps_(desktop_rate(fps)), id_(id) {
    format_.fps = fps_;
    format_.frame_time = fps_ == 50.0 ? 200000 : 10'000'000 / 60;
    format_.pixel_format = directshow_capture::VideoFormat::PixelFormat::bgra;
    for (const auto& monitor : list_monitors()) {
        if (monitor.id == id) device_name_ = monitor.name;
    }
    if (device_name_.empty()) throw std::runtime_error("That monitor is not available");
    std::wprintf(L"Desktop: %ls\n", device_name_.c_str());

    runner_ = std::make_unique<Runner>();
    runner_->ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    runner_->stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    runner_->image_ready = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!runner_->ready || !runner_->stop || !runner_->image_ready || !hooks_.scale_done) {
        throw std::runtime_error("CreateEvent failed");
    }

    std::promise<std::string> opened;
    auto ready = opened.get_future();
    runner_->thread = std::thread([this, &opened] { run(opened); });
    const std::string error = ready.get();
    if (!error.empty()) {
        runner_->stopping.store(true);
        SetEvent(runner_->stop);
        if (runner_->thread.joinable()) runner_->thread.join();
        throw std::runtime_error(error);
    }
}

DesktopCapture::~DesktopCapture() {
    if (!runner_) return;
    runner_->stopping.store(true);
    if (runner_->stop) SetEvent(runner_->stop);
    if (runner_->thread.joinable()) runner_->thread.join();
}

bool DesktopCapture::wait_for_sample(std::vector<uint8_t>& dest, LONGLONG& timestamp, DWORD timeout_ms) {
    if (!runner_ || WaitForSingleObject(runner_->ready, timeout_ms) != WAIT_OBJECT_0) return false;
    std::lock_guard lock(runner_->mutex);
    if (!runner_->have) return false;
    dest.assign(runner_->latest.begin(), runner_->latest.end());
    timestamp = runner_->timestamp;
    runner_->have = false;
    return true;
}

uint64_t DesktopCapture::take_replaced_samples() {
    return runner_ ? runner_->replaced.exchange(0) : 0;
}

uint64_t DesktopCapture::take_samples() {
    return runner_ ? runner_->samples.exchange(0) : 0;
}

uint64_t DesktopCapture::take_gaps() {
    return 0;
}

bool DesktopCapture::live() const {
    return runner_ && runner_->live.load();
}

void DesktopCapture::show_preview(const std::vector<uint8_t>& shown, unsigned width, unsigned height) {
    if (shown.size() < static_cast<size_t>(width) * height * 4u) return;
    std::lock_guard lock(runner_->mutex);
    if (runner_->stopping.load()) return;
    if (runner_->have) runner_->replaced.fetch_add(1);
    runner_->latest.assign(shown.begin(), shown.end());
    runner_->timestamp = -1;
    runner_->have = true;
    SetEvent(runner_->ready);
}

void DesktopCapture::run(std::promise<std::string>& opened) {
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    TickClock clock;

    std::promise<std::string> grab_opened;
    auto grab_ready = grab_opened.get_future();
    std::thread grabber([this, &grab_opened] {
        const HRESULT com = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        const bool com_started = SUCCEEDED(com);
        struct ComGuard {
            bool started;
            ~ComGuard() { if (started) CoUninitialize(); }
        } com_guard{com_started && com != S_FALSE};
        try {
            ComPtr<IDXGIFactory1> factory;
            if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory)))) throw std::runtime_error("No graphics factory");
            ComPtr<IDXGIOutput1> output1;
            ComPtr<IDXGIAdapter> adapter;
            for (UINT adapter_index = 0; !output1 && factory; ++adapter_index) {
                ComPtr<IDXGIAdapter1> adapter1;
                if (factory->EnumAdapters1(adapter_index, &adapter1) == DXGI_ERROR_NOT_FOUND) break;
                for (UINT output_index = 0;; ++output_index) {
                    ComPtr<IDXGIOutput> output;
                    if (adapter1->EnumOutputs(output_index, &output) == DXGI_ERROR_NOT_FOUND) break;
                    DXGI_OUTPUT_DESC desc{};
                    if (FAILED(output->GetDesc(&desc))) continue;
                    if (std::wstring(kPrefix) + desc.DeviceName != id_) continue;
                    if (FAILED(output.As(&output1))) throw std::runtime_error("This monitor cannot be duplicated");
                    adapter = adapter1;
                    break;
                }
            }
            if (!output1) throw std::runtime_error("That monitor is not available");

            ComPtr<ID3D11Device> device;
            ComPtr<ID3D11DeviceContext> context;
            const D3D_FEATURE_LEVEL levels[] = {D3D_FEATURE_LEVEL_11_0, D3D_FEATURE_LEVEL_10_0};
            if (FAILED(D3D11CreateDevice(adapter.Get(), D3D_DRIVER_TYPE_UNKNOWN, nullptr,
                                         D3D11_CREATE_DEVICE_BGRA_SUPPORT, levels, 2, D3D11_SDK_VERSION,
                                         &device, nullptr, &context))) {
                throw std::runtime_error("Couldn't open the monitor");
            }
            ComPtr<IDXGIOutputDuplication> duplication;
            if (FAILED(output1->DuplicateOutput(device.Get(), &duplication))) {
                throw std::runtime_error("Couldn't duplicate that monitor");
            }
            // Windows takes the duplication away when the display changes: a new
            // mode, a fullscreen game, a security prompt. Ask for it again until it
            // comes back, and keep sending the last picture meanwhile.
            const auto reopen = [&] {
                runner_->live.store(false);
                duplication.Reset();
                std::printf("Desktop capture lost: waiting for the monitor\n");
                while (!runner_->stopping.load()) {
                    if (WaitForSingleObject(runner_->stop, 100) == WAIT_OBJECT_0) return false;
                    if (SUCCEEDED(output1->DuplicateOutput(device.Get(), &duplication))) {
                        std::printf("Desktop capture back\n");
                        runner_->live.store(true);
                        return true;
                    }
                }
                return false;
            };
            DXGI_OUTDUPL_DESC dup_desc{};
            duplication->GetDesc(&dup_desc);
            if (dup_desc.ModeDesc.Format != DXGI_FORMAT_B8G8R8A8_UNORM) {
                throw std::runtime_error("This monitor is not an 8-bit desktop");
            }
            format_.width = static_cast<int>(dup_desc.ModeDesc.Width);
            format_.height = static_cast<int>(dup_desc.ModeDesc.Height);
            format_.pixel_format = directshow_capture::VideoFormat::PixelFormat::bgra;
            std::printf("Desktop format: BGRA %dx%d at %.2f fps\n", format_.width, format_.height, format_.fps);
            runner_->live.store(true);
            grab_opened.set_value("");

            std::vector<uint8_t> desktop;
            std::vector<uint8_t> shown;
            unsigned width = 0, height = 0;
            ComPtr<ID3D11Texture2D> staging;
            Pointer pointer;
            while (!runner_->stopping.load()) {
                DXGI_OUTDUPL_FRAME_INFO frame{};
                ComPtr<IDXGIResource> resource;
                const HRESULT acquired = duplication->AcquireNextFrame(15, &frame, &resource);
                if (runner_->stopping.load()) break;
                if (acquired == DXGI_ERROR_WAIT_TIMEOUT) continue;
                if (FAILED(acquired)) {
                    if (!reopen()) break;
                    continue;
                }
                if (frame.AccumulatedFrames > 0 && resource) {
                    ComPtr<ID3D11Texture2D> texture;
                    if (SUCCEEDED(resource.As(&texture))) {
                        copy_desktop(device.Get(), context.Get(), texture.Get(), staging, desktop, width, height);
                    }
                }
                if (frame.LastMouseUpdateTime.QuadPart != 0 || frame.PointerShapeBufferSize != 0) {
                    read_pointer(duplication.Get(), frame, pointer);
                }
                duplication->ReleaseFrame();
                if (desktop.empty() || width < 2 || height < 1) continue;
                shown = desktop;
                if (pointer.visible) {
                    paint_cursor(shown.data(), static_cast<int>(width), static_cast<int>(height), pointer.shape,
                                 pointer.info, pointer.x - pointer.info.HotSpot.x,
                                 pointer.y - pointer.info.HotSpot.y);
                }
                std::lock_guard lock(runner_->image_mutex);
                runner_->image.swap(shown);
                runner_->image_w = width;
                runner_->image_h = height;
                ++runner_->image_serial;
                SetEvent(runner_->image_ready);
            }
            runner_->live.store(false);
        } catch (const std::exception& error) {
            runner_->live.store(false);
            try {
                grab_opened.set_value(error.what());
            } catch (const std::future_error&) {
                std::printf("Desktop capture stopped: %s\n", error.what());
            }
        }
    });
    struct JoinGrabber {
        std::thread& thread;
        std::atomic<bool>& stopping;
        HANDLE stop;
        ~JoinGrabber() {
            stopping.store(true);
            if (stop) SetEvent(stop);
            if (thread.joinable()) thread.join();
        }
    } join_grabber{grabber, runner_->stopping, runner_->stop};

    try {
        const std::string error = grab_ready.get();
        if (!error.empty()) {
            opened.set_value(error);
            return;
        }
        opened.set_value("");

        // Pictures scale as they arrive. This tick only releases one that has
        // already finished, which keeps the stream at the selected rate.
        const auto tick = std::chrono::nanoseconds(1'000'000'000 / static_cast<int>(fps_));
        auto next_tick = std::chrono::steady_clock::now() + tick;
        bool tick_armed = clock.arm(next_tick);
        std::vector<uint8_t> frame;
        unsigned width = 0, height = 0;
        uint64_t seen = 0;
        uint64_t submitted = 0;
        bool scaling = false;
        bool have_finished = false;
        HANDLE handles[4] = {runner_->stop, runner_->image_ready, hooks_.scale_done, clock.handle()};
        const DWORD handle_count = clock.handle() ? 4u : 3u;

        while (!runner_->stopping.load()) {
            DWORD wait_ms = INFINITE;
            if (!tick_armed) {
                const auto left = next_tick - std::chrono::steady_clock::now();
                if (left <= std::chrono::steady_clock::duration::zero()) {
                    wait_ms = 0;
                } else {
                    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(left).count();
                    wait_ms = static_cast<DWORD>(ms > 0 ? ms : 0);
                }
            }
            const DWORD woke = WaitForMultipleObjects(handle_count, handles, FALSE, wait_ms);
            if (woke == WAIT_OBJECT_0 || woke == WAIT_FAILED) break;
            if (runner_->stopping.load()) break;

            bool swapped = false;
            {
                std::lock_guard lock(runner_->image_mutex);
                if (runner_->image_serial != seen && !runner_->image.empty()) {
                    frame.swap(runner_->image);
                    width = runner_->image_w;
                    height = runner_->image_h;
                    seen = runner_->image_serial;
                    format_.width = static_cast<int>(width);
                    format_.height = static_cast<int>(height);
                    swapped = true;
                }
            }

            const auto now = std::chrono::steady_clock::now();
            if (now >= next_tick) {
                // The timer stays signaled until a wait consumes it. Re-arming
                // the next tick also clears a signal this wait did not take.
                if (woke != WAIT_OBJECT_0 + 3 && clock.handle()) WaitForSingleObject(clock.handle(), 0);
                if (hooks_.tick) hooks_.tick();
                runner_->samples.fetch_add(1);
                if (hooks_.release) hooks_.release();
                next_tick += tick;
                const auto after = std::chrono::steady_clock::now();
                if (next_tick < after) next_tick = after + tick;
                tick_armed = clock.arm(next_tick);
            }

            if (scaling) {
                if (hooks_.busy && hooks_.busy()) continue;
                scaling = false;
                have_finished = true;
            }
            if (!swapped && seen == submitted) continue;
            if (seen == 0 || seen == submitted || frame.empty() || width < 2 || height < 1) continue;
            const auto left = next_tick - std::chrono::steady_clock::now();
            if (have_finished && hooks_.budget && left < hooks_.budget()) continue;
            if (!hooks_.start || !hooks_.start(frame.data(), static_cast<long>(width) * 4, width, height)) {
                continue;
            }
            submitted = seen;
            show_preview(frame, width, height);
            scaling = hooks_.busy && hooks_.busy();
            if (!scaling) have_finished = true;
        }
    } catch (const std::exception& error) {
        try {
            opened.set_value(error.what());
        } catch (const std::future_error&) {
            std::printf("Desktop capture stopped: %s\n", error.what());
        }
    }
}

} // namespace desktop_capture
