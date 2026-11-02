#pragma once

#include "field_prep.h"

#include <cstdint>
#include <vector>

namespace groovy {

// The scaler's row work in AVX2, giving exactly the bytes the plain code in FieldPrep
// does, about 3.5x as fast at 720p. Only call anything here if cpu_has_avx2().
bool cpu_has_avx2();

class Avx2Rows {
public:
    // For a span of a source row scaled to `dest_w` pixels. False if that takes more
    // filter taps than this handles, as with sources over about 5K.
    bool setup(unsigned src_width, unsigned src_x0, unsigned span_w, unsigned dest_w);

    void begin_row();
    // Adds source row `y`, with a weight out of 256.
    void add_row(const Sample& sample, unsigned y, unsigned weight);
    // Scales the rows added and writes `dest_w` RGB565 pixels.
    void finish_row(bool rgb, uint16_t* out);

private:
    void load_row(const Sample& sample, unsigned y);

    static constexpr unsigned kMaxTaps = 8;
    unsigned src_width_ = 0, src_x0_ = 0, span_w_ = 0, dest_w_ = 0, taps_ = 0;
    std::vector<uint8_t> plane_[3], averaged_[3], scaled_[3];
    std::vector<uint16_t> acc_[3], row_;
    std::vector<int> first_, weight_[kMaxTaps];
};

} // namespace groovy
