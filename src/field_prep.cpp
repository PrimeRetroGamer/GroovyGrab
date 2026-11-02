#include "field_prep.h"

#include "field_prep_avx2.h"
#include "lz4.h"
#include "video_view.h"

#include <algorithm>
#include <cstddef>
#include <cstring>

namespace groovy {

namespace {

constexpr unsigned kOutputWidth = 720;

double ms_between(std::chrono::steady_clock::time_point a,
                  std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

uint8_t clamp_byte(int value) {
    return static_cast<uint8_t>(std::clamp(value, 0, 255));
}

uint16_t pack565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// BT.601 limited range.
uint16_t yuv_to_565(uint8_t luma, uint8_t cb, uint8_t cr) {
    const int c = 298 * std::max(0, luma - 16);
    const int u = cb - 128;
    const int v = cr - 128;
    return pack565(clamp_byte((c + 409 * v + 128) >> 8),
                   clamp_byte((c - 100 * u - 208 * v + 128) >> 8),
                   clamp_byte((c + 516 * u + 128) >> 8));
}

// The row decoders unpack to Y,U,V triplets. The scaler averages them like
// RGB and yuv_to_565() converts each output pixel, which is equivalent because
// the conversion is linear apart from clamping, and far cheaper at 720p.

void decode_yuy2_row(const uint8_t* row, unsigned width, uint8_t* yuv) {
    for (unsigned x = 0; x < width; x += 2) {
        const uint8_t* in = row + (x >> 1) * 4;
        uint8_t* out = yuv + x * 3u;
        out[0] = in[0];
        out[1] = in[1];
        out[2] = in[3];
        out[3] = in[2];
        out[4] = in[1];
        out[5] = in[3];
    }
}

void decode_uyvy_row(const uint8_t* row, unsigned width, uint8_t* yuv) {
    for (unsigned x = 0; x < width; x += 2) {
        const uint8_t* in = row + (x >> 1) * 4;
        uint8_t* out = yuv + x * 3u;
        out[0] = in[1];
        out[1] = in[0];
        out[2] = in[2];
        out[3] = in[3];
        out[4] = in[0];
        out[5] = in[2];
    }
}

void decode_nv12_row(const uint8_t* y_row, const uint8_t* uv_row, unsigned width,
                     uint8_t* yuv) {
    for (unsigned x = 0; x < width; x += 2) {
        const uint8_t u = uv_row[x], v = uv_row[x + 1];
        uint8_t* out = yuv + x * 3u;
        out[0] = y_row[x];
        out[1] = u;
        out[2] = v;
        out[3] = y_row[x + 1];
        out[4] = u;
        out[5] = v;
    }
}

void decode_i420_row(const uint8_t* y_row, const uint8_t* u_row, const uint8_t* v_row,
                     unsigned width, uint8_t* yuv) {
    for (unsigned x = 0; x < width; x += 2) {
        const uint8_t u = u_row[x / 2], v = v_row[x / 2];
        uint8_t* out = yuv + x * 3u;
        out[0] = y_row[x];
        out[1] = u;
        out[2] = v;
        out[3] = y_row[x + 1];
        out[4] = u;
        out[5] = v;
    }
}

} // namespace

FieldPrep::FieldPrep() {
    if (cpu_has_avx2()) avx2_ = std::make_unique<Avx2Rows>();
}

FieldPrep::~FieldPrep() = default;

bool FieldPrep::prepare(const Sample& sample, VideoMode mode, const ViewSettings& view,
                        PreparedField& out) {
    out.convert_ms = out.lz4_ms = 0.0;
    for (uint8_t parity = 0; parity < (is_interlaced(mode) ? 2 : 1); ++parity) {
        if (!prepare_field(sample, mode, view, parity, true, out)) return false;
    }
    return true;
}

bool FieldPrep::prepare_field(const Sample& sample, VideoMode mode, const ViewSettings& view, uint8_t parity,
                              bool timed, PreparedField& out) {
    if (!sample.data || sample.width < 2 || sample.height < 1) return false;
    const unsigned out_h = output_height(mode);
    const int raw_size = static_cast<int>(kOutputWidth * out_h * sizeof(uint16_t));
    pixels_.resize(kOutputWidth * out_h);
    const auto t0 = std::chrono::steady_clock::now();
    convert(sample, out_h, is_interlaced(mode), parity, view);
    const auto t1 = std::chrono::steady_clock::now();
    auto& data = out.data[parity];
    data.resize(static_cast<size_t>(LZ4_compressBound(raw_size)));
    out.size[parity] = LZ4_compress_default(reinterpret_cast<const char*>(pixels_.data()),
                                            data.data(), raw_size,
                                            static_cast<int>(data.size()));
    if (timed) {
        out.convert_ms += ms_between(t0, t1);
        out.lz4_ms += ms_between(t1, std::chrono::steady_clock::now());
    }
    if (out.size[parity] <= 0) return false;
    out.ready[parity].store(true);
    return true;
}

// Source row `y` as triplets: RGB for decoded MJPEG, otherwise Y,U,V.
const uint8_t* FieldPrep::row_triplets(const Sample& sample, unsigned y) {
    const uint8_t* row = sample.data + static_cast<ptrdiff_t>(y) * sample.pitch;
    const unsigned width = sample.width;
    const ptrdiff_t luma_bytes = static_cast<ptrdiff_t>(sample.height) * sample.pitch;
    switch (sample.pixels) {
    case SamplePixels::Rgb:
        return row;
    case SamplePixels::Yuy2:
        decode_yuy2_row(row, width, row_.data());
        break;
    case SamplePixels::Uyvy:
        decode_uyvy_row(row, width, row_.data());
        break;
    case SamplePixels::Nv12:
        decode_nv12_row(row, sample.data + luma_bytes + static_cast<ptrdiff_t>(y / 2u) * sample.pitch,
                        width, row_.data());
        break;
    case SamplePixels::I420: {
        const long chroma_pitch = sample.pitch / 2;
        const uint8_t* u_plane = sample.data + luma_bytes;
        const uint8_t* v_plane = u_plane + static_cast<ptrdiff_t>(sample.height / 2u) * chroma_pitch;
        const ptrdiff_t chroma_row = static_cast<ptrdiff_t>(y / 2u) * chroma_pitch;
        decode_i420_row(row, u_plane + chroma_row, v_plane + chroma_row, width, row_.data());
        break;
    }
    }
    return row_.data();
}

void FieldPrep::convert(const Sample& sample, unsigned out_h, bool field_only,
                        uint8_t source_parity, const ViewSettings& view) {
    const auto map = video_view::make(sample.width, sample.height, kOutputWidth, out_h,
                                      view.letterbox, view.crop_h, view.crop_v);
    unsigned field_first = map.src_y0;
    unsigned field_step = 1;
    if (field_only) {
        field_step = 2;
        if ((field_first & 1u) != source_parity) field_first += 1u;
        if (field_first >= map.src_y1) field_first = map.src_y0;
    }
    const unsigned field_n = field_first < map.src_y1
                                 ? (map.src_y1 - 1u - field_first) / field_step + 1u
                                 : 1u;
    const unsigned dest_span = map.dst_y1 - map.dst_y0;
    const unsigned dest_w = map.dst_x1 - map.dst_x0;
    const unsigned span_w = map.src_x1 > map.src_x0 ? map.src_x1 - map.src_x0 : 1u;
    const size_t acc_n = static_cast<size_t>(span_w) * 3u;

    const bool fast = avx2_ && avx2_allowed_ && avx2_->setup(sample.width, map.src_x0, span_w, dest_w);
    if (!fast) {
        row_.resize(static_cast<size_t>(sample.width) * 3u);
        acc_.resize(acc_n);
        averaged_.resize(acc_n);
        scaled_.resize(static_cast<size_t>(dest_w) * 3u);
    }

    // Accumulate the contributing rows at source width, then scale horizontally
    // once per output line.
    for (unsigned y = 0; y < out_h; ++y) {
        uint16_t* output = pixels_.data() + static_cast<size_t>(y) * kOutputWidth;
        if (y < map.dst_y0 || y >= map.dst_y1) {
            std::memset(output, 0, kOutputWidth * sizeof(uint16_t));
            continue;
        }
        const unsigned dest_pos = y - map.dst_y0;
        const uint64_t start_fp = static_cast<uint64_t>(dest_pos) * field_n * 256ull / dest_span;
        uint64_t end_fp = dest_pos + 1u == dest_span
                              ? static_cast<uint64_t>(field_n) << 8
                              : static_cast<uint64_t>(dest_pos + 1u) * field_n * 256ull / dest_span;
        if (end_fp <= start_fp) end_fp = start_fp + 256u;
        const unsigned i0 = std::min(static_cast<unsigned>(start_fp >> 8), field_n - 1);
        const unsigned i1 = std::min(static_cast<unsigned>((end_fp - 1u) >> 8), field_n - 1);

        // Row weights are normalised to sum to exactly 256 (error diffusion), so
        // the average is a shift and the accumulator fits 16 bits.
        const auto row_weight = [&](unsigned index) {
            const uint64_t lo = std::max<uint64_t>(start_fp, uint64_t(index) << 8);
            const uint64_t hi = std::min<uint64_t>(end_fp, uint64_t(index + 1u) << 8);
            return hi > lo ? static_cast<unsigned>(hi - lo) : 0u;
        };
        unsigned weight_sum = 0;
        for (unsigned index = i0; index <= i1; ++index) weight_sum += row_weight(index);
        if (!weight_sum) weight_sum = 1;

        if (fast) avx2_->begin_row();
        else std::fill(acc_.begin(), acc_.end(), uint16_t{0});
        unsigned cumulative = 0, given = 0;
        for (unsigned index = i0; index <= i1; ++index) {
            const unsigned weight = row_weight(index);
            if (!weight) continue;
            cumulative += weight;
            const unsigned target = (cumulative * 256u + weight_sum / 2u) / weight_sum;
            const uint16_t w = static_cast<uint16_t>(target - given);
            given = target;
            if (!w) continue;
            const unsigned sy = std::min(sample.height - 1, field_first + index * field_step);
            if (fast) {
                avx2_->add_row(sample, sy, w);
                continue;
            }
            const uint8_t* row = row_triplets(sample, sy) + static_cast<size_t>(map.src_x0) * 3u;
            for (size_t i = 0; i < acc_n; ++i) {
                acc_[i] = static_cast<uint16_t>(acc_[i] + row[i] * w);
            }
        }
        if (fast) {
            std::memset(output, 0, map.dst_x0 * sizeof(uint16_t));
            avx2_->finish_row(sample.pixels == SamplePixels::Rgb, output + map.dst_x0);
            std::memset(output + map.dst_x1, 0, (kOutputWidth - map.dst_x1) * sizeof(uint16_t));
            continue;
        }
        for (size_t i = 0; i < acc_n; ++i) {
            averaged_[i] = static_cast<uint8_t>((acc_[i] + 128u) >> 8);
        }
        scale_row(averaged_.data(), span_w, scaled_.data(), dest_w);

        std::memset(output, 0, map.dst_x0 * sizeof(uint16_t));
        const uint8_t* in = scaled_.data();
        uint16_t* out = output + map.dst_x0;
        if (sample.pixels == SamplePixels::Rgb) {
            for (unsigned x = 0; x < dest_w; ++x, in += 3) out[x] = pack565(in[0], in[1], in[2]);
        } else {
            for (unsigned x = 0; x < dest_w; ++x, in += 3) out[x] = yuv_to_565(in[0], in[1], in[2]);
        }
        std::memset(output + map.dst_x1, 0, (kOutputWidth - map.dst_x1) * sizeof(uint16_t));
    }
}

// Horizontal box filter. Taps are computed once per geometry and normalised to
// sum to 256, so each output pixel is a shift.
void FieldPrep::scale_row(const uint8_t* in, unsigned src_w, uint8_t* out, unsigned dst_w) {
    if (src_w == dst_w) {
        std::memcpy(out, in, dst_w * 3u);
        return;
    }
    if (tap_src_w_ != src_w || tap_dst_w_ != dst_w) {
        tap_src_w_ = src_w;
        tap_dst_w_ = dst_w;
        tap_first_.assign(dst_w, 0);
        tap_start_.assign(dst_w + 1u, 0);
        tap_weight_.clear();
        for (unsigned x = 0; x < dst_w; ++x) {
            const uint64_t start_fp = static_cast<uint64_t>(x) * src_w * 256ull / dst_w;
            uint64_t end_fp = x + 1u == dst_w
                                  ? static_cast<uint64_t>(src_w) << 8
                                  : static_cast<uint64_t>(x + 1u) * src_w * 256ull / dst_w;
            if (end_fp <= start_fp) end_fp = start_fp + 256u;
            const unsigned i0 = std::min(static_cast<unsigned>(start_fp >> 8), src_w - 1);
            const unsigned i1 = std::min(static_cast<unsigned>((end_fp - 1u) >> 8), src_w - 1);
            tap_first_[x] = i0;
            unsigned sum = 0;
            for (unsigned sx = i0; sx <= i1; ++sx) {
                const uint64_t lo = std::max<uint64_t>(start_fp, uint64_t(sx) << 8);
                const uint64_t hi = std::min<uint64_t>(end_fp, uint64_t(sx + 1u) << 8);
                const unsigned w = hi > lo ? static_cast<unsigned>(hi - lo) : 0u;
                tap_weight_.push_back(static_cast<uint16_t>(w));
                sum += w;
            }
            if (!sum) sum = 1;
            tap_start_[x + 1u] = static_cast<uint32_t>(tap_weight_.size());
            unsigned cumulative = 0, given = 0;
            for (uint32_t k = tap_start_[x]; k < tap_start_[x + 1u]; ++k) {
                cumulative += tap_weight_[k];
                const unsigned target = (cumulative * 256u + sum / 2u) / sum;
                tap_weight_[k] = static_cast<uint16_t>(target - given);
                given = target;
            }
        }
    }
    for (unsigned x = 0; x < dst_w; ++x) {
        const uint8_t* taps = in + static_cast<size_t>(tap_first_[x]) * 3u;
        const uint16_t* w = tap_weight_.data() + tap_start_[x];
        const unsigned n = tap_start_[x + 1u] - tap_start_[x];
        unsigned a = 0, b = 0, c = 0;
        for (unsigned k = 0; k < n; ++k, taps += 3) {
            a += taps[0] * w[k];
            b += taps[1] * w[k];
            c += taps[2] * w[k];
        }
        out[x * 3u + 0] = static_cast<uint8_t>((a + 128u) >> 8);
        out[x * 3u + 1] = static_cast<uint8_t>((b + 128u) >> 8);
        out[x * 3u + 2] = static_cast<uint8_t>((c + 128u) >> 8);
    }
}

} // namespace groovy
