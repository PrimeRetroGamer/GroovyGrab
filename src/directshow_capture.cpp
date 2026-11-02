#include "directshow_capture.h"

#include <cfgmgr32.h>
#include <dvdmedia.h>
#include <initguid.h>
#include <devpkey.h>
#include <mmsystem.h>
#include <oleauto.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cwctype>
#include <stdexcept>
#include <utility>

namespace directshow_capture {

namespace {

constexpr CLSID kClsidSampleGrabber = {
    0xc1f400a0, 0x3f08, 0x11d3, {0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37}};
constexpr CLSID kClsidNullRenderer = {
    0xc1f400a4, 0x3f08, 0x11d3, {0x9f, 0x0b, 0x00, 0x60, 0x08, 0x03, 0x9e, 0x37}};

// FOURCC 'I420'. uuids.h only defines the identical-layout 'IYUV'.
constexpr GUID kMediaSubtypeI420 = {
    0x30323449, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

using PixelFormat = VideoFormat::PixelFormat;

bool is_i420_subtype(const GUID& subtype) {
    return subtype == kMediaSubtypeI420 || subtype == MEDIASUBTYPE_IYUV;
}

bool planar_420(PixelFormat format) {
    return format == PixelFormat::nv12 || format == PixelFormat::i420;
}

// The path of a wave input device. Audio monikers have an index, not a DevicePath.
std::wstring wave_in_path(LONG wave_in_id) {
    constexpr UINT kQueryDeviceInterface = DRV_RESERVED + 12; // DRV_QUERYDEVICEINTERFACE
    wchar_t path[512]{};
    const MMRESULT result = waveInMessage(reinterpret_cast<HWAVEIN>(static_cast<UINT_PTR>(wave_in_id)),
                                          kQueryDeviceInterface, reinterpret_cast<DWORD_PTR>(path), sizeof(path));
    return result == MMSYSERR_NOERROR ? path : L"";
}

// Every part of one physical device has the same container ID. Built-in and software
// devices all share the system's, which says nothing about whether they belong together.
std::wstring container_of(const std::wstring& device_path) {
    static const GUID kSystemContainer = {0x00000000, 0x0000, 0x0000, {0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff}};
    DEVPROPTYPE type = 0;
    wchar_t instance[MAX_DEVICE_ID_LEN]{};
    ULONG size = sizeof(instance);
    if (device_path.empty() ||
        CM_Get_Device_Interface_PropertyW(device_path.c_str(), &DEVPKEY_Device_InstanceId, &type,
                                          reinterpret_cast<PBYTE>(instance), &size, 0) != CR_SUCCESS) {
        return {};
    }
    DEVINST node = 0;
    if (CM_Locate_DevNodeW(&node, instance, CM_LOCATE_DEVNODE_NORMAL) != CR_SUCCESS) return {};
    GUID container{};
    size = sizeof(container);
    if (CM_Get_DevNode_PropertyW(node, &DEVPKEY_Device_ContainerId, &type,
                                 reinterpret_cast<PBYTE>(&container), &size, 0) != CR_SUCCESS ||
        container == kSystemContainer) {
        return {};
    }
    wchar_t text[64]{};
    return StringFromGUID2(container, text, 64) ? text : L"";
}

std::vector<Device> list_devices(REFCLSID category) {
    std::vector<Device> devices;
    for_each_device(category, [&](const std::wstring& name, const std::wstring& path, IMoniker*) {
        devices.push_back({name, name, path, container_of(path)});
    });
    for (auto& device : devices) {
        const auto same_name = [&](const Device& other) { return other.name == device.name; };
        if (!device.path.empty() && std::count_if(devices.begin(), devices.end(), same_name) > 1) {
            device.id = device.path;
        }
    }
    return devices;
}

ComPtr<IPin> find_pin(IBaseFilter* filter, PIN_DIRECTION wanted) {
    ComPtr<IEnumPins> pins;
    check(filter->EnumPins(&pins), "EnumPins");
    for (;;) {
        ComPtr<IPin> pin;
        ULONG fetched = 0;
        if (pins->Next(1, &pin, &fetched) != S_OK) break;
        PIN_DIRECTION direction{};
        if (SUCCEEDED(pin->QueryDirection(&direction)) && direction == wanted) return pin;
    }
    throw std::runtime_error("Filter pin not found");
}

struct PixelFormatNames {
    PixelFormat format;
    const char* name;
    const char* setting;
};
constexpr PixelFormatNames kPixelFormats[] = {
    {PixelFormat::yuy2, "YUY2", "yuy2"},
    {PixelFormat::uyvy, "UYVY", "uyvy"},
    {PixelFormat::mjpeg, "MJPEG", "mjpeg"},
    {PixelFormat::nv12, "NV12", "nv12"},
    {PixelFormat::i420, "I420", "i420"},
};

const PixelFormatNames& pixel_format_names(PixelFormat format) {
    for (const auto& names : kPixelFormats) {
        if (names.format == format) return names;
    }
    return kPixelFormats[0];
}

bool parse_video_format(const AM_MEDIA_TYPE& type, VideoFormat& out) {
    if (type.majortype != MEDIATYPE_Video ||
        (type.subtype != MEDIASUBTYPE_YUY2 && type.subtype != MEDIASUBTYPE_UYVY &&
         type.subtype != MEDIASUBTYPE_MJPG && type.subtype != MEDIASUBTYPE_NV12 &&
         !is_i420_subtype(type.subtype))) {
        return false;
    }
    LONG width = 0;
    LONG height = 0;
    REFERENCE_TIME frame_time = 0;
    if (type.formattype == FORMAT_VideoInfo && type.cbFormat >= sizeof(VIDEOINFOHEADER)) {
        const auto* info = reinterpret_cast<const VIDEOINFOHEADER*>(type.pbFormat);
        width = info->bmiHeader.biWidth;
        height = std::abs(info->bmiHeader.biHeight);
        frame_time = info->AvgTimePerFrame;
    } else if (type.formattype == FORMAT_VideoInfo2 && type.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        const auto* info = reinterpret_cast<const VIDEOINFOHEADER2*>(type.pbFormat);
        width = info->bmiHeader.biWidth;
        height = std::abs(info->bmiHeader.biHeight);
        frame_time = info->AvgTimePerFrame;
    }
    if (width < 2 || (width & 1) || height < 1) return false;
    out.width = static_cast<int>(width);
    out.height = static_cast<int>(height);
    out.frame_time = frame_time;
    out.fps = frame_time > 0 ? 10'000'000.0 / static_cast<double>(frame_time) : 0.0;
    out.pixel_format = type.subtype == MEDIASUBTYPE_MJPG   ? PixelFormat::mjpeg
                       : type.subtype == MEDIASUBTYPE_NV12 ? PixelFormat::nv12
                       : type.subtype == MEDIASUBTYPE_UYVY ? PixelFormat::uyvy
                       : is_i420_subtype(type.subtype)     ? PixelFormat::i420
                                                           : PixelFormat::yuy2;
    return out.fps == 0.0 || (out.fps > 1.0 && out.fps < 250.0);
}

bool same_format(const VideoFormat& a, const VideoFormat& b) {
    return a.width == b.width && a.height == b.height && a.pixel_format == b.pixel_format &&
           std::abs(a.fps - b.fps) < 0.15;
}

void set_avg_time(AM_MEDIA_TYPE& type, REFERENCE_TIME frame_time) {
    if (frame_time <= 0 || !type.pbFormat) return;
    if (type.formattype == FORMAT_VideoInfo && type.cbFormat >= sizeof(VIDEOINFOHEADER)) {
        reinterpret_cast<VIDEOINFOHEADER*>(type.pbFormat)->AvgTimePerFrame = frame_time;
    } else if (type.formattype == FORMAT_VideoInfo2 && type.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        reinterpret_cast<VIDEOINFOHEADER2*>(type.pbFormat)->AvgTimePerFrame = frame_time;
    }
}

void add_unique_format(std::vector<VideoFormat>& formats, const VideoFormat& format) {
    if (format.width < 2 || format.height < 1 || format.fps <= 1.0 || format.fps >= 250.0) return;
    for (const auto& seen : formats) {
        if (same_format(seen, format)) return;
    }
    formats.push_back(format);
}

VIDEO_STREAM_CONFIG_CAPS read_stream_caps(const std::vector<BYTE>& data) {
    VIDEO_STREAM_CONFIG_CAPS caps{};
    if (data.size() >= sizeof(caps)) std::memcpy(&caps, data.data(), sizeof(caps));
    if (caps.MinFrameInterval > 0 && caps.MaxFrameInterval > 0 &&
        caps.MinFrameInterval > caps.MaxFrameInterval) {
        std::swap(caps.MinFrameInterval, caps.MaxFrameInterval);
    }
    return caps;
}

bool interval_in_range(REFERENCE_TIME value, const VIDEO_STREAM_CONFIG_CAPS& caps) {
    if (value <= 0 || caps.MinFrameInterval <= 0 || caps.MaxFrameInterval <= 0) return false;
    return value >= caps.MinFrameInterval && value <= caps.MaxFrameInterval;
}

// A capability can be a range of frame intervals: list the common rates in it.
void add_rates_from_caps(std::vector<VideoFormat>& formats, const VideoFormat& base,
                         const VIDEO_STREAM_CONFIG_CAPS& caps) {
    add_unique_format(formats, base);
    static constexpr REFERENCE_TIME kIntervals[] = {
        83333,  83350,  100000, 166667, 166833, 200000,
        333333, 333667, 400000, 416667, 417083, 500000,
    };
    VIDEO_STREAM_CONFIG_CAPS range = caps;
    if (range.MinFrameInterval <= 0) range.MinFrameInterval = base.frame_time;
    if (range.MaxFrameInterval <= 0) range.MaxFrameInterval = base.frame_time;
    const auto add_interval = [&](REFERENCE_TIME interval) {
        VideoFormat extra = base;
        extra.frame_time = interval;
        extra.fps = 10'000'000.0 / static_cast<double>(interval);
        add_unique_format(formats, extra);
    };
    for (REFERENCE_TIME interval : kIntervals) {
        if (interval_in_range(interval, range)) add_interval(interval);
    }
    if (range.MinFrameInterval > 0 && range.MinFrameInterval == range.MaxFrameInterval) {
        add_interval(range.MinFrameInterval);
    }
}

ComPtr<IBaseFilter> bind_source(const std::wstring& device_name) {
    std::wstring chosen;
    const auto moniker = find_device(CLSID_VideoInputDeviceCategory, device_name, chosen);
    ComPtr<IBaseFilter> source;
    check(moniker->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&source)),
          "BindToObject(capture device)");
    return source;
}

} // namespace

void check(HRESULT hr, const char* what) {
    if (FAILED(hr)) {
        char text[192];
        std::snprintf(text, sizeof(text), "%s failed (HRESULT 0x%08lx)", what,
                      static_cast<unsigned long>(hr));
        throw std::runtime_error(text);
    }
}

bool contains_insensitive(std::wstring value, std::wstring needle) {
    std::transform(value.begin(), value.end(), value.begin(), towlower);
    std::transform(needle.begin(), needle.end(), needle.begin(), towlower);
    return value.find(needle) != std::wstring::npos;
}

void delete_media_type(AM_MEDIA_TYPE* type) {
    if (!type) return;
    if (type->cbFormat) CoTaskMemFree(type->pbFormat);
    if (type->pUnk) type->pUnk->Release();
    CoTaskMemFree(type);
}

void for_each_device(REFCLSID category,
                     const std::function<void(const std::wstring&, const std::wstring&, IMoniker*)>& fn) {
    ComPtr<ICreateDevEnum> device_enum;
    check(CoCreateInstance(CLSID_SystemDeviceEnum, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&device_enum)),
          "CoCreateInstance(SystemDeviceEnum)");
    ComPtr<IEnumMoniker> monikers;
    if (device_enum->CreateClassEnumerator(category, &monikers, 0) != S_OK) return;
    for (;;) {
        ComPtr<IMoniker> moniker;
        ULONG fetched = 0;
        if (monikers->Next(1, &moniker, &fetched) != S_OK) break;
        ComPtr<IPropertyBag> properties;
        if (FAILED(moniker->BindToStorage(nullptr, nullptr, IID_PPV_ARGS(&properties)))) continue;
        VARIANT value;
        VariantInit(&value);
        if (SUCCEEDED(properties->Read(L"FriendlyName", &value, nullptr)) && value.vt == VT_BSTR) {
            const std::wstring name = value.bstrVal;
            std::wstring path;
            VariantClear(&value);
            VariantInit(&value);
            if (SUCCEEDED(properties->Read(L"DevicePath", &value, nullptr)) && value.vt == VT_BSTR) {
                path = value.bstrVal;
            } else {
                VariantClear(&value);
                VariantInit(&value);
                if (SUCCEEDED(properties->Read(L"WaveInId", &value, nullptr)) && value.vt == VT_I4) {
                    path = wave_in_path(value.lVal);
                }
            }
            fn(name, path, moniker.Get());
        }
        VariantClear(&value);
    }
}

ComPtr<IMoniker> find_device(REFCLSID category, const std::wstring& id, std::wstring& chosen_name) {
    ComPtr<IMoniker> selected;
    for_each_device(category, [&](const std::wstring& name, const std::wstring& path, IMoniker* moniker) {
        if (!selected && (path == id || name == id)) {
            selected = moniker;
            chosen_name = name;
        }
    });
    if (!selected) throw std::runtime_error("Device not found");
    return selected;
}

void GrabberCallback::shutdown() {
    std::unique_lock lock(mutex_);
    stopping_ = true;
    idle_.wait(lock, [this] { return active_callbacks_ == 0; });
}

HRESULT STDMETHODCALLTYPE GrabberCallback::QueryInterface(REFIID iid, void** object) {
    if (!object) return E_POINTER;
    if (iid == IID_IUnknown || iid == __uuidof(ISampleGrabberCB)) {
        *object = static_cast<ISampleGrabberCB*>(this);
        AddRef();
        return S_OK;
    }
    *object = nullptr;
    return E_NOINTERFACE;
}

ULONG STDMETHODCALLTYPE GrabberCallback::Release() {
    const ULONG remaining = --references_;
    if (!remaining) delete this;
    return remaining;
}

HRESULT STDMETHODCALLTYPE GrabberCallback::SampleCB(double, IMediaSample* sample) {
    if (!sample) return E_POINTER;
    {
        std::lock_guard lock(mutex_);
        if (stopping_) return S_FALSE;
        ++active_callbacks_;
    }
    HRESULT result = S_OK;
    // No exception may cross the COM boundary into a capture driver thread.
    try {
        BYTE* data = nullptr;
        if (FAILED(sample->GetPointer(&data)) || !data) {
            result = E_POINTER;
        } else {
            result = on_sample(data, sample->GetActualDataLength(), sample);
        }
    } catch (...) {
        result = E_FAIL;
    }
    std::lock_guard lock(mutex_);
    if (--active_callbacks_ == 0) idle_.notify_all();
    return result;
}

void GrabberGraph::add_source(IMoniker* device) {
    check(CoCreateInstance(CLSID_FilterGraph, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&graph_)),
          "CoCreateInstance(FilterGraph)");
    check(device->BindToObject(nullptr, nullptr, IID_PPV_ARGS(&source_)), "BindToObject(device)");
    check(graph_->AddFilter(source_.Get(), L"Capture source"), "AddFilter(capture source)");
}

void GrabberGraph::connect(IPin* capture_pin, const GUID& major, const GUID& subtype,
                           ComPtr<GrabberCallback> callback) {
    check(CoCreateInstance(kClsidSampleGrabber, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&grabber_filter_)),
          "CoCreateInstance(SampleGrabber)");
    check(grabber_filter_.As(&grabber_), "QueryInterface(ISampleGrabber)");
    check(graph_->AddFilter(grabber_filter_.Get(), L"Sample Grabber"), "AddFilter(Sample Grabber)");

    AM_MEDIA_TYPE wanted{};
    wanted.majortype = major;
    wanted.subtype = subtype;
    wanted.formattype = GUID_NULL;
    check(grabber_->SetMediaType(&wanted), "SampleGrabber SetMediaType");
    check(grabber_->SetOneShot(FALSE), "SampleGrabber SetOneShot");
    check(grabber_->SetBufferSamples(FALSE), "SampleGrabber SetBufferSamples");

    callback_ = std::move(callback);
    check(grabber_->SetCallback(callback_.Get(), 0), "SampleGrabber SetCallback");

    check(CoCreateInstance(kClsidNullRenderer, nullptr, CLSCTX_INPROC_SERVER,
                           IID_PPV_ARGS(&null_renderer_)),
          "CoCreateInstance(NullRenderer)");
    check(graph_->AddFilter(null_renderer_.Get(), L"Null Renderer"), "AddFilter(NullRenderer)");

    grabber_input_ = find_pin(grabber_filter_.Get(), PINDIR_INPUT);
    const ComPtr<IPin> grabber_output = find_pin(grabber_filter_.Get(), PINDIR_OUTPUT);
    const ComPtr<IPin> null_input = find_pin(null_renderer_.Get(), PINDIR_INPUT);
    check(graph_->ConnectDirect(capture_pin, grabber_input_.Get(), nullptr),
          "ConnectDirect(capture to grabber)");
    check(graph_->ConnectDirect(grabber_output.Get(), null_input.Get(), nullptr),
          "ConnectDirect(grabber to null renderer)");
}

void GrabberGraph::start(bool free_running) {
    if (free_running) {
        ComPtr<IMediaFilter> media_filter;
        check(graph_.As(&media_filter), "QueryInterface(IMediaFilter)");
        check(media_filter->SetSyncSource(nullptr), "SetSyncSource(nullptr)");
    }
    check(graph_.As(&control_), "QueryInterface(IMediaControl)");
    check(control_->Run(), "DirectShow graph Run");
}

DWORD GrabberGraph::connected_image_size() const {
    AM_MEDIA_TYPE type{};
    if (FAILED(grabber_->GetConnectedMediaType(&type))) return 0;
    DWORD size = 0;
    if (type.formattype == FORMAT_VideoInfo && type.cbFormat >= sizeof(VIDEOINFOHEADER)) {
        size = reinterpret_cast<const VIDEOINFOHEADER*>(type.pbFormat)->bmiHeader.biSizeImage;
    } else if (type.formattype == FORMAT_VideoInfo2 && type.cbFormat >= sizeof(VIDEOINFOHEADER2)) {
        size = reinterpret_cast<const VIDEOINFOHEADER2*>(type.pbFormat)->bmiHeader.biSizeImage;
    }
    if (type.cbFormat) CoTaskMemFree(type.pbFormat);
    if (type.pUnk) type.pUnk->Release();
    return size;
}

GrabberGraph::~GrabberGraph() {
    if (callback_) callback_->shutdown();
    if (grabber_) grabber_->SetCallback(nullptr, 0);
    if (control_) control_->Stop();
}

const char* pixel_format_name(PixelFormat format) {
    return pixel_format_names(format).name;
}

const char* pixel_format_setting(PixelFormat format) {
    return pixel_format_names(format).setting;
}

PixelFormat pixel_format_from_setting(const std::string& value) {
    for (const auto& names : kPixelFormats) {
        if (value == names.setting) return names.format;
    }
    return PixelFormat::yuy2;
}

long source_stride(const VideoFormat& format, DWORD image_size) {
    constexpr long kMaxRowPadding = 256;
    const bool planar = planar_420(format.pixel_format);
    const long tight = planar ? format.width : format.width * 2;
    const long rows = planar ? format.height + format.height / 2 : format.height;
    if (format.pixel_format == PixelFormat::mjpeg || rows <= 0 || !image_size ||
        image_size % static_cast<DWORD>(rows) != 0) {
        return tight;
    }
    // Some drivers declare the 16-bit buffer size for 4:2:0 too.
    if (planar && image_size == static_cast<DWORD>(format.width) * format.height * 2u) return tight;
    const long stride = static_cast<long>(image_size) / rows;
    if (stride < tight || stride >= tight + kMaxRowPadding || (planar && (stride & 1))) return tight;
    return stride;
}

void pack_rows_tight(const uint8_t* source, long stride, const VideoFormat& format, uint8_t* dest) {
    const size_t width = static_cast<size_t>(format.width);
    const size_t height = static_cast<size_t>(format.height);
    const auto copy_rows = [&](size_t row_bytes, size_t rows, long pitch) {
        for (size_t y = 0; y < rows; ++y) {
            std::memcpy(dest, source, row_bytes);
            dest += row_bytes;
            source += pitch;
        }
    };
    switch (format.pixel_format) {
    case PixelFormat::yuy2:
    case PixelFormat::uyvy:
        copy_rows(width * 2, height, stride);
        break;
    case PixelFormat::nv12:
        copy_rows(width, height + height / 2, stride);
        break;
    case PixelFormat::i420:
        copy_rows(width, height, stride);
        copy_rows(width / 2, height, stride / 2); // U then V, h/2 rows each
        break;
    case PixelFormat::mjpeg:
        break;
    }
}

std::vector<Device> list_video_devices() {
    return list_devices(CLSID_VideoInputDeviceCategory);
}

std::vector<Device> list_audio_devices() {
    return list_devices(CLSID_AudioInputDeviceCategory);
}

std::vector<VideoFormat> list_video_formats(const std::wstring& device_name) {
    std::vector<VideoFormat> formats;
    try {
        const ComPtr<IBaseFilter> source = bind_source(device_name);
        for_each_stream_config(source.Get(), [&](const ComPtr<IPin>&, IAMStreamConfig* config) {
            int count = 0, caps_size = 0;
            if (FAILED(config->GetNumberOfCapabilities(&count, &caps_size)) || caps_size <= 0) {
                return false;
            }
            std::vector<BYTE> caps_data(static_cast<size_t>(caps_size));
            for (int index = 0; index < count; ++index) {
                AM_MEDIA_TYPE* type = nullptr;
                if (FAILED(config->GetStreamCaps(index, &type, caps_data.data())) || !type) {
                    delete_media_type(type);
                    continue;
                }
                VideoFormat format{};
                if (parse_video_format(*type, format)) {
                    const auto caps = read_stream_caps(caps_data);
                    if (format.frame_time <= 0 && caps.MinFrameInterval > 0) {
                        format.frame_time = caps.MinFrameInterval;
                        format.fps = 10'000'000.0 / static_cast<double>(format.frame_time);
                    }
                    add_rates_from_caps(formats, format, caps);
                }
                delete_media_type(type);
            }
            return false;
        });
    } catch (const std::exception& error) {
        std::printf("Capture format list failed: %s\n", error.what());
    }
    std::sort(formats.begin(), formats.end(), [](const VideoFormat& a, const VideoFormat& b) {
        if (a.width != b.width) return a.width < b.width;
        if (a.height != b.height) return a.height < b.height;
        if (a.pixel_format != b.pixel_format) return a.pixel_format < b.pixel_format;
        return a.fps < b.fps;
    });
    return formats;
}

// The pin's current format if it has one, else 720x480/60, else the first listed.
VideoFormat default_video_format(const std::wstring& device_name) {
    VideoFormat current{};
    try {
        const ComPtr<IBaseFilter> source = bind_source(device_name);
        for_each_stream_config(source.Get(), [&](const ComPtr<IPin>&, IAMStreamConfig* config) {
            AM_MEDIA_TYPE* type = nullptr;
            if (FAILED(config->GetFormat(&type)) || !type) return false;
            VideoFormat format{};
            const bool ok = parse_video_format(*type, format) && format.fps > 1.0;
            delete_media_type(type);
            if (ok) current = format;
            return ok;
        });
    } catch (const std::exception& error) {
        std::printf("Capture default format failed: %s\n", error.what());
    }
    if (current.width > 0) return current;
    const auto formats = list_video_formats(device_name);
    for (const auto& format : formats) {
        if (format.width == 720 && format.height == 480 && std::abs(format.fps - 60.0) < 1.0) {
            return format;
        }
    }
    return formats.empty() ? VideoFormat{} : formats.front();
}

class Capture::Callback final : public GrabberCallback {
public:
    Callback(const VideoFormat& format, FrameFn on_frame)
        : on_frame_(std::move(on_frame)), format_(format),
          compressed_(format.pixel_format == PixelFormat::mjpeg) {
        if (format.width < 2 || format.height < 1) throw std::runtime_error("Invalid capture size");
        const size_t pixels = static_cast<size_t>(format.width) * format.height;
        max_bytes_ = pixels * 2u;
        if (planar_420(format.pixel_format)) {
            pitch_ = format.width;
            bytes_ = pixels * 3u / 2u;
        } else if (!compressed_) {
            pitch_ = format.width * 2;
            bytes_ = pixels * 2u;
        }
        source_bytes_ = bytes_;
        ready_ = CreateEvent(nullptr, FALSE, FALSE, nullptr);
        if (!ready_) throw std::runtime_error("CreateEvent failed");
    }

    ~Callback() override { CloseHandle(ready_); }

    bool wait_for_latest(std::vector<uint8_t>& dest, LONGLONG& timestamp, DWORD timeout_ms) {
        const DWORD result = WaitForSingleObject(ready_, timeout_ms);
        if (result == WAIT_FAILED) throw std::runtime_error("Frame wait failed");
        if (result != WAIT_OBJECT_0) return false;
        std::lock_guard lock(mutex_);
        if (!have_sample_) return false;
        dest.assign(latest_.begin(), latest_.end());
        timestamp = latest_timestamp_;
        have_sample_ = false;
        return true;
    }

    uint64_t take_replaced_samples() { return replaced_samples_.exchange(0); }
    uint64_t take_samples() { return samples_.exchange(0); }
    uint64_t take_gaps() { return gaps_.exchange(0); }

    // Set before the graph starts.
    void set_source_stride(long stride) {
        stride_ = stride;
        if (pitch_ && stride != pitch_) {
            source_bytes_ = bytes_ / static_cast<size_t>(pitch_) * static_cast<size_t>(stride);
            tight_.resize(bytes_);
        }
    }

protected:
    HRESULT on_sample(BYTE* source, long length, IMediaSample* sample) override {
        // The driver's thread, at normal priority: under load, samples then arrive late
        // enough for the rate lock to see a source that changes rate.
        if (!thread_raised_) {
            thread_raised_ = true;
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        }
        const bool size_ok = compressed_
                                 ? length >= 4 && static_cast<size_t>(length) <= max_bytes_
                                 : static_cast<size_t>(length) >= source_bytes_;
        if (!size_ok) return E_INVALIDARG;
        const long kept = compressed_ ? length : static_cast<long>(bytes_);
        const uint8_t* data = source;
        if (!tight_.empty()) {
            pack_rows_tight(source, stride_, format_, tight_.data());
            data = tight_.data();
        }
        on_frame_(data, kept, pitch_, format_);
        REFERENCE_TIME start = 0, stop = 0;
        if (FAILED(sample->GetTime(&start, &stop))) start = -1;
        count_sample(start);
        {
            std::lock_guard lock(mutex_);
            if (stopping_) return S_FALSE;
            if (have_sample_) ++replaced_samples_;
            latest_.assign(data, data + kept);
            latest_timestamp_ = start;
            have_sample_ = true;
        }
        SetEvent(ready_);
        return S_OK;
    }

private:
    // Counted here because the preview can miss samples, as while dragging the window.
    void count_sample(REFERENCE_TIME timestamp) {
        ++samples_;
        const auto expected = static_cast<LONGLONG>(10'000'000.0 / std::max(format_.fps, 1.0));
        if (timestamp < 0) timestamp = previous_timestamp_ < 0 ? 0 : previous_timestamp_ + expected;
        if (previous_timestamp_ >= 0 && timestamp - previous_timestamp_ > expected + expected / 2) {
            gaps_ += static_cast<uint64_t>((timestamp - previous_timestamp_ + expected / 2) / expected - 1);
        }
        previous_timestamp_ = timestamp;
    }

    FrameFn on_frame_;
    bool thread_raised_ = false;
    std::atomic<uint64_t> samples_{0};
    std::atomic<uint64_t> gaps_{0};
    LONGLONG previous_timestamp_ = -1;
    HANDLE ready_{};
    VideoFormat format_;
    std::vector<uint8_t> latest_;
    LONGLONG latest_timestamp_ = 0;
    bool have_sample_ = false;
    std::atomic<uint64_t> replaced_samples_{0};
    bool compressed_;
    long pitch_ = 0;           // 0 and no fixed size when compressed
    size_t bytes_ = 0;
    size_t source_bytes_ = 0;
    size_t max_bytes_ = 0;
    long stride_ = 0;
    std::vector<uint8_t> tight_; // only used when the driver pads rows
};

Capture::Capture(const std::wstring& device_name, VideoFormat format, FrameFn on_frame) {
    const auto moniker = find_device(CLSID_VideoInputDeviceCategory, device_name, device_name_);
    std::wprintf(L"Capture device: %ls\n", device_name_.c_str());
    format_ = format.width >= 2 && format.height >= 1 ? format : default_video_format(device_name);

    graph_.add_source(moniker.Get());
    ComPtr<IPin> capture_pin;
    GUID subtype = MEDIASUBTYPE_YUY2;
    select_format(capture_pin, subtype);
    request_single_buffer(capture_pin.Get());

    ComPtr<Callback> callback;
    callback.Attach(new Callback(format_, std::move(on_frame)));
    callback_ = callback.Get();
    graph_.connect(capture_pin.Get(), MEDIATYPE_Video, subtype, std::move(callback));
    const DWORD image_size = graph_.connected_image_size();
    const long stride = source_stride(format_, image_size);
    callback_->set_source_stride(stride);
    std::printf("Capture stride: %ld\n", stride);
    force_single_buffer(image_size);
    graph_.start(true);
}

bool Capture::wait_for_sample(std::vector<uint8_t>& dest, LONGLONG& timestamp, DWORD timeout_ms) {
    return callback_->wait_for_latest(dest, timestamp, timeout_ms);
}

uint64_t Capture::take_replaced_samples() {
    return callback_->take_replaced_samples();
}

uint64_t Capture::take_samples() {
    return callback_->take_samples();
}

uint64_t Capture::take_gaps() {
    return callback_->take_gaps();
}

// Applies `type` to the pin and takes on the size it gives. False if the driver refused.
bool Capture::adopt_format(const ComPtr<IPin>& pin, IAMStreamConfig* config, AM_MEDIA_TYPE& type,
                           const VideoFormat& format, ComPtr<IPin>& capture_pin, GUID& subtype) {
    if (FAILED(config->SetFormat(&type))) return false;
    const bool same_size = format.width == format_.width && format.height == format_.height;
    if (!same_size || format_.fps <= 1.0) format_.fps = format.fps;
    format_.width = format.width;
    format_.height = format.height;
    capture_pin = pin;
    subtype = type.subtype;
    return true;
}

// On one pin: an exact match or a capability whose interval range covers it, otherwise
// the first capability of the same pixel format.
bool Capture::select_on_pin(const ComPtr<IPin>& pin, IAMStreamConfig* config, ComPtr<IPin>& capture_pin,
                            GUID& subtype) {
    int count = 0, caps_size = 0;
    if (FAILED(config->GetNumberOfCapabilities(&count, &caps_size)) || caps_size <= 0) return false;
    std::vector<BYTE> caps_data(static_cast<size_t>(caps_size));
    AM_MEDIA_TYPE* fallback = nullptr;
    bool selected = false;
    for (int index = 0; index < count && !selected; ++index) {
        AM_MEDIA_TYPE* type = nullptr;
        VideoFormat format{};
        if (FAILED(config->GetStreamCaps(index, &type, caps_data.data())) || !type ||
            !parse_video_format(*type, format) || format.pixel_format != format_.pixel_format) {
            delete_media_type(type);
            continue;
        }
        const bool same_size = format.width == format_.width && format.height == format_.height;
        if (same_size && (same_format(format, format_) ||
                          interval_in_range(format_.frame_time, read_stream_caps(caps_data)))) {
            set_avg_time(*type, format_.frame_time);
            selected = adopt_format(pin, config, *type, format, capture_pin, subtype);
        }
        if (!selected && !fallback) {
            fallback = type;
        } else {
            delete_media_type(type);
        }
    }
    if (!selected && fallback) {
        VideoFormat format{};
        parse_video_format(*fallback, format);
        if (format.width == format_.width && format.height == format_.height) {
            set_avg_time(*fallback, format_.frame_time);
        }
        selected = adopt_format(pin, config, *fallback, format, capture_pin, subtype);
    }
    delete_media_type(fallback);
    return selected;
}

void Capture::select_format(ComPtr<IPin>& capture_pin, GUID& subtype) {
    bool selected = false;
    for_each_stream_config(graph_.source(), [&](const ComPtr<IPin>& pin, IAMStreamConfig* config) {
        selected = select_on_pin(pin, config, capture_pin, subtype);
        return selected;
    });
    if (!selected) {
        char text[96];
        std::snprintf(text, sizeof(text), "Device does not offer %s %dx%d at %.2f fps",
                      pixel_format_name(format_.pixel_format), format_.width, format_.height, format_.fps);
        throw UnsupportedFormat(text);
    }
    std::printf("Capture format: %s %dx%d at %.2f fps\n", pixel_format_name(format_.pixel_format),
                format_.width, format_.height, format_.fps);
}

// One driver buffer: anything more is a capture latency queue.
void Capture::request_single_buffer(IPin* capture_pin) {
    ComPtr<IAMBufferNegotiation> negotiation;
    if (FAILED(capture_pin->QueryInterface(IID_PPV_ARGS(&negotiation)))) return;
    ALLOCATOR_PROPERTIES request{};
    request.cBuffers = 1;
    request.cbBuffer = format_.width * format_.height * 2;
    request.cbAlign = 1;
    negotiation->SuggestAllocatorProperties(&request);
}

void Capture::force_single_buffer(DWORD image_size) {
    ComPtr<IMemInputPin> memory_input;
    if (FAILED(graph_.grabber_input()->QueryInterface(IID_PPV_ARGS(&memory_input)))) return;
    ComPtr<IMemAllocator> allocator;
    if (FAILED(memory_input->GetAllocator(&allocator))) return;
    ALLOCATOR_PROPERTIES request{};
    request.cBuffers = 1;
    request.cbBuffer = std::max(format_.width * format_.height * 2, static_cast<int>(image_size));
    request.cbAlign = 1;
    ALLOCATOR_PROPERTIES actual{};
    if (SUCCEEDED(allocator->SetProperties(&request, &actual)) ||
        SUCCEEDED(allocator->GetProperties(&actual))) {
        buffer_count_ = actual.cBuffers;
    }
    std::printf("Capture buffers: %ld\n", buffer_count_);
}

} // namespace directshow_capture
