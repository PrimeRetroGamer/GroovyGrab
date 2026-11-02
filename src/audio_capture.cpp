#include "audio_capture.h"

#include <mmreg.h>

#include <cstdio>
#include <stdexcept>
#include <utility>
#include <vector>

namespace directshow_capture {

namespace {

bool is_pcm_format(const AM_MEDIA_TYPE& type, DWORD sample_rate) {
    if (type.majortype != MEDIATYPE_Audio || type.subtype != MEDIASUBTYPE_PCM ||
        type.formattype != FORMAT_WaveFormatEx || type.cbFormat < sizeof(WAVEFORMATEX) ||
        !type.pbFormat) {
        return false;
    }
    const auto* wave = reinterpret_cast<const WAVEFORMATEX*>(type.pbFormat);
    return wave->wFormatTag == WAVE_FORMAT_PCM && wave->nChannels == 2 &&
           wave->wBitsPerSample == 16 && wave->nSamplesPerSec == sample_rate;
}

// Sets `rate` on the first output pin offering it; returns that pin or null.
ComPtr<IPin> select_pcm_format(IBaseFilter* source, DWORD rate) {
    ComPtr<IPin> found;
    for_each_stream_config(source, [&](const ComPtr<IPin>& pin, IAMStreamConfig* config) {
        int count = 0, caps_size = 0;
        if (FAILED(config->GetNumberOfCapabilities(&count, &caps_size)) || caps_size <= 0) return false;
        std::vector<BYTE> caps_data(static_cast<size_t>(caps_size));
        for (int index = 0; index < count; ++index) {
            AM_MEDIA_TYPE* type = nullptr;
            bool selected = false;
            if (SUCCEEDED(config->GetStreamCaps(index, &type, caps_data.data())) && type &&
                is_pcm_format(*type, rate)) {
                selected = SUCCEEDED(config->SetFormat(type));
            }
            delete_media_type(type);
            if (selected) {
                found = pin;
                return true;
            }
        }
        return false;
    });
    return found;
}

class Callback final : public GrabberCallback {
public:
    explicit Callback(AudioCapture::Sink sink) : sink_(std::move(sink)) {}

protected:
    HRESULT on_sample(BYTE* data, long length, IMediaSample*) override {
        if (length > 0) sink_(data, static_cast<size_t>(length));
        return S_OK;
    }

private:
    AudioCapture::Sink sink_;
};

} // namespace

AudioCapture::AudioCapture(const std::wstring& id, Sink sink) {
    std::wstring name;
    const auto moniker = find_device(CLSID_AudioInputDeviceCategory, id, name);
    graph_.add_source(moniker.Get());

    ComPtr<IPin> capture_pin;
    for (const DWORD rate : {48000ul, 44100ul}) {
        capture_pin = select_pcm_format(graph_.source(), rate);
        if (capture_pin) {
            sample_rate_ = rate;
            break;
        }
    }
    if (!capture_pin) {
        throw UnsupportedFormat("Device does not offer 48 or 44.1 kHz stereo 16-bit PCM");
    }

    ComPtr<IAMBufferNegotiation> negotiation;
    if (SUCCEEDED(capture_pin.As(&negotiation))) {
        ALLOCATOR_PROPERTIES request{};
        request.cBuffers = 4;
        request.cbBuffer = static_cast<long>((sample_rate_ / 100) * 4); // 10 ms
        request.cbAlign = 1;
        negotiation->SuggestAllocatorProperties(&request);
    }

    ComPtr<Callback> callback;
    callback.Attach(new Callback(std::move(sink)));
    graph_.connect(capture_pin.Get(), MEDIATYPE_Audio, MEDIASUBTYPE_PCM, std::move(callback));
    graph_.start(false);
    std::wprintf(L"Audio device: %ls\n", name.c_str());
    std::printf("Audio format: %u Hz stereo s16\n", sample_rate_);
}

} // namespace directshow_capture
