#pragma once

#include "directshow_capture.h"

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace directshow_capture {

// HDMI capture audio is a separate DirectShow device. 48 kHz stereo s16 is
// preferred; 44.1 kHz is accepted as a fallback.
class AudioCapture {
public:
    using Sink = std::function<void(const uint8_t*, size_t)>;

    AudioCapture(const std::wstring& id, Sink sink);

    uint32_t sample_rate() const { return sample_rate_; }

private:
    GrabberGraph graph_;
    uint32_t sample_rate_ = 0;
};

} // namespace directshow_capture
