#pragma once

#include "video_mode.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <vector>

namespace groovy {

class Avx2Rows;

// Pixel layout of a capture sample.
//   Yuy2: packed 4:2:2, Y0 U Y1 V.
//   Uyvy: packed 4:2:2, U Y0 V Y1.
//   Rgb:  packed RGB24 (decoded MJPEG).
//   Bgra: packed B8G8R8A8, the desktop's own bytes. Alpha is ignored.
//   Nv12: Y plane, then interleaved UV at half height, same row pitch.
//   I420: Y plane, then U and V planes at half width and height.
enum class SamplePixels { Yuy2, Uyvy, Rgb, Nv12, I420, Bgra };

struct Sample {
    const uint8_t* data = nullptr;
    long pitch = 0; // luma row stride for the planar formats
    unsigned width = 0;
    unsigned height = 0;
    SamplePixels pixels = SamplePixels::Yuy2;
};

struct ViewSettings {
    bool letterbox = false;
    int crop_h = 0;
    int crop_v = 0;
};

// One capture sample, ready to send as LZ4 RGB565. Interlaced modes hold both
// parities: which one a blit carries is only known once it is numbered. One is
// usually enough, so it goes out while the other is still being prepared: `ready`
// says which of them can be read.
struct PreparedField {
    std::array<std::vector<char>, 2> data; // [source parity]; [0] if progressive
    std::array<int, 2> size{};
    std::array<std::atomic<bool>, 2> ready{};
    double decode_ms = 0.0, convert_ms = 0.0, lz4_ms = 0.0;
    std::chrono::steady_clock::time_point captured{}, picked{};
    std::chrono::steady_clock::time_point source{}; // the sample's own timestamp
    uint64_t handoff_epoch = 0; // which capture graph this sample came from
};

// Scales a capture sample to 720 x field height (separable box filter), packs
// it to RGB565 and LZ4-compresses it. Not thread safe; owned by the prep thread.
class FieldPrep {
public:
    // Every parity the mode has.
    bool prepare(const Sample& sample, VideoMode mode, const ViewSettings& view,
                 PreparedField& out);
    // One parity. `timed` records how long it took, and has to be false if another thread
    // may already be reading `out`.
    bool prepare_field(const Sample& sample, VideoMode mode, const ViewSettings& view, uint8_t parity,
                       bool timed, PreparedField& out);

    // Uses AVX2 if the CPU has it. Both ways give the same bytes, so off is only for testing.
    void set_avx2_allowed(bool allowed) { avx2_allowed_ = allowed; }

    FieldPrep();
    ~FieldPrep();
    FieldPrep(const FieldPrep&) = delete;
    FieldPrep& operator=(const FieldPrep&) = delete;

private:
    void convert(const Sample& sample, unsigned out_h, bool field_only,
                 uint8_t source_parity, const ViewSettings& view);
    const uint8_t* row_triplets(const Sample& sample, unsigned y);
    void scale_row(const uint8_t* in, unsigned src_w, uint8_t* out, unsigned dst_w);

    std::unique_ptr<Avx2Rows> avx2_; // null if the CPU lacks AVX2
    bool avx2_allowed_ = true;

    std::vector<uint16_t> pixels_;  // RGB565 output
    std::vector<uint8_t> row_;      // source row as Y,U,V triplets
    std::vector<uint16_t> acc_;     // weighted sum of source rows
    std::vector<uint8_t> averaged_; // acc_ / 256
    std::vector<uint8_t> scaled_;   // averaged_ scaled to the output width

    // Horizontal filter taps, rebuilt when the geometry changes.
    unsigned tap_src_w_ = 0;
    unsigned tap_dst_w_ = 0;
    std::vector<unsigned> tap_first_;
    std::vector<uint32_t> tap_start_;
    std::vector<uint16_t> tap_weight_;
};

} // namespace groovy
