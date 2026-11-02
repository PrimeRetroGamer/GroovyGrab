#pragma once

#include <windows.h>
#include <dshow.h>
#include <wrl/client.h>

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace directshow_capture {

using Microsoft::WRL::ComPtr;

// Qedit.h was removed from current Windows SDKs. These are the original,
// published Sample Grabber interfaces.
struct __declspec(uuid("0579154A-2B53-4994-B0D0-E773148EFF85"))
ISampleGrabberCB : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SampleCB(double sample_time, IMediaSample* sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE BufferCB(double sample_time, BYTE* buffer, long length) = 0;
};

struct __declspec(uuid("6B652FFF-11FE-4FCE-92AD-0266B5D7C78F"))
ISampleGrabber : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE SetOneShot(BOOL one_shot) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetMediaType(const AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetConnectedMediaType(AM_MEDIA_TYPE* type) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetBufferSamples(BOOL buffer_samples) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentBuffer(long* size, long* buffer) = 0;
    virtual HRESULT STDMETHODCALLTYPE GetCurrentSample(IMediaSample** sample) = 0;
    virtual HRESULT STDMETHODCALLTYPE SetCallback(ISampleGrabberCB* callback, long method) = 0;
};

// The device works, but does not offer the format asked for.
struct UnsupportedFormat : std::runtime_error {
    using std::runtime_error::runtime_error;
};

void check(HRESULT hr, const char* what);
bool contains_insensitive(std::wstring value, std::wstring needle);
void delete_media_type(AM_MEDIA_TYPE* type);

// Calls fn(pin, config) for every configurable output pin until it returns true.
template <typename Fn>
void for_each_stream_config(IBaseFilter* source, Fn&& fn) {
    ComPtr<IEnumPins> pins;
    if (FAILED(source->EnumPins(&pins))) return;
    for (;;) {
        ComPtr<IPin> pin;
        ULONG fetched = 0;
        if (pins->Next(1, &pin, &fetched) != S_OK) return;
        PIN_DIRECTION direction{};
        if (FAILED(pin->QueryDirection(&direction)) || direction != PINDIR_OUTPUT) continue;
        ComPtr<IAMStreamConfig> config;
        if (FAILED(pin.As(&config))) continue;
        if (fn(pin, config.Get())) return;
    }
}

// Calls fn(friendly name, device path, moniker) for every device in a DirectShow
// category. Audio devices have their wave device's path. The path is empty for devices
// that have none.
void for_each_device(REFCLSID category,
                     const std::function<void(const std::wstring&, const std::wstring&, IMoniker*)>& fn);

// First device of the category with this exact path or name.
ComPtr<IMoniker> find_device(REFCLSID category, const std::wstring& id, std::wstring& chosen_name);

// COM boilerplate and safe shutdown for a Sample Grabber callback. Derived
// classes handle samples in on_sample(), which never runs after shutdown().
class GrabberCallback : public ISampleGrabberCB {
public:
    virtual ~GrabberCallback() = default;

    // Prevents new work and waits for a callback already issued by the driver:
    // SetCallback(nullptr) alone does not guarantee it has stopped.
    void shutdown();

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** object) override;
    ULONG STDMETHODCALLTYPE AddRef() override { return ++references_; }
    ULONG STDMETHODCALLTYPE Release() override;
    HRESULT STDMETHODCALLTYPE SampleCB(double, IMediaSample* sample) override;
    HRESULT STDMETHODCALLTYPE BufferCB(double, BYTE*, long) override { return E_NOTIMPL; }

protected:
    virtual HRESULT on_sample(BYTE* data, long length, IMediaSample* sample) = 0;

    std::mutex mutex_;
    bool stopping_ = false;

private:
    std::atomic<ULONG> references_{1};
    std::condition_variable idle_;
    unsigned active_callbacks_ = 0;
};

// source -> Sample Grabber -> Null Renderer. Only native samples: DirectShow
// never inserts a decoder or colour converter.
class GrabberGraph {
public:
    GrabberGraph() = default;
    GrabberGraph(const GrabberGraph&) = delete;
    GrabberGraph& operator=(const GrabberGraph&) = delete;
    ~GrabberGraph();

    void add_source(IMoniker* device);
    IBaseFilter* source() const { return source_.Get(); }

    // Connects `capture_pin` through the grabber.
    void connect(IPin* capture_pin, const GUID& major, const GUID& subtype,
                 ComPtr<GrabberCallback> callback);
    IPin* grabber_input() const { return grabber_input_.Get(); }

    // biSizeImage of the connected video format; 0 if unknown.
    DWORD connected_image_size() const;

    // `free_running`: no reference clock, samples are delivered as they arrive.
    void start(bool free_running);

private:
    // Declared first: outlives the graph if the constructor exits early.
    ComPtr<GrabberCallback> callback_;
    ComPtr<IGraphBuilder> graph_;
    ComPtr<IMediaControl> control_;
    ComPtr<IBaseFilter> source_;
    ComPtr<IBaseFilter> grabber_filter_;
    ComPtr<ISampleGrabber> grabber_;
    ComPtr<IBaseFilter> null_renderer_;
    ComPtr<IPin> grabber_input_;
};

struct VideoFormat {
    enum class PixelFormat { yuy2, mjpeg, nv12, i420, uyvy };

    int width = 0;
    int height = 0;
    double fps = 0.0;
    REFERENCE_TIME frame_time = 0;
    PixelFormat pixel_format = PixelFormat::yuy2;
};

const char* pixel_format_name(VideoFormat::PixelFormat format);    // for display
const char* pixel_format_setting(VideoFormat::PixelFormat format); // for the settings file
VideoFormat::PixelFormat pixel_format_from_setting(const std::string& value); // YUY2 if unknown

// Row stride of a sample from the image size the driver declares. Falls back to
// tightly packed rows if that size does not describe padded rows.
long source_stride(const VideoFormat& format, DWORD image_size);

// Copies a padded uncompressed sample to tightly packed rows.
void pack_rows_tight(const uint8_t* source, long stride, const VideoFormat& format, uint8_t* dest);

struct Device {
    std::wstring name;
    std::wstring id;        // the name, or the device path if another device shares that name
    std::wstring path;      // empty for devices that have none
    std::wstring container; // the physical device it is part of; empty if unknown or built in
};
std::vector<Device> list_video_devices();
std::vector<Device> list_audio_devices();
std::vector<VideoFormat> list_video_formats(const std::wstring& device_name);
VideoFormat default_video_format(const std::wstring& device_name);

class Capture {
public:
    // Called on the driver's thread. size is the byte count; pitch is the row
    // stride (luma stride for 4:2:0), or 0 for a JPEG. Uncompressed rows are
    // always tightly packed here, whatever the driver delivers.
    using FrameFn = std::function<void(const uint8_t* data, long size, long pitch,
                                       const VideoFormat& format)>;

    Capture(const std::wstring& device_name, VideoFormat format, FrameFn on_frame);

    // Waits for a sample newer than the last one taken and copies it for the
    // preview. Timestamp is -1 if the driver gave none.
    bool wait_for_sample(std::vector<uint8_t>& dest, LONGLONG& timestamp, DWORD timeout_ms);
    uint64_t take_replaced_samples();
    // Samples the device delivered since the last call, and frames its timestamps skipped.
    uint64_t take_samples();
    uint64_t take_gaps();

    long buffer_count() const { return buffer_count_; }
    const std::wstring& device_name() const { return device_name_; }
    const VideoFormat& format() const { return format_; }

private:
    class Callback;

    bool adopt_format(const ComPtr<IPin>& pin, IAMStreamConfig* config, AM_MEDIA_TYPE& type,
                      const VideoFormat& format, ComPtr<IPin>& capture_pin, GUID& subtype);
    bool select_on_pin(const ComPtr<IPin>& pin, IAMStreamConfig* config, ComPtr<IPin>& capture_pin, GUID& subtype);
    void select_format(ComPtr<IPin>& capture_pin, GUID& subtype);
    void request_single_buffer(IPin* capture_pin);
    void force_single_buffer(DWORD image_size);

    std::wstring device_name_;
    VideoFormat format_{};
    long buffer_count_ = -1;
    GrabberGraph graph_;
    Callback* callback_ = nullptr; // owned by graph_
};

} // namespace directshow_capture
