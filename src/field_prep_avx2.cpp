#include "field_prep_avx2.h"

#include <immintrin.h>
#include <intrin.h>

#include <algorithm>
#include <cstddef>
#include <cstring>

// Built without /arch:AVX2, so that only what is called from here needs the CPU to have it.

namespace groovy {

namespace {

// Room past the end of each row for whole vectors.
constexpr size_t kPadding = 64;

const __m128i* as_m128(const void* p) {
    return static_cast<const __m128i*>(p);
}

const __m256i* as_m256(const void* p) {
    return static_cast<const __m256i*>(p);
}

void store8(void* p, __m128i v) {
    _mm_storel_epi64(static_cast<__m128i*>(p), v);
}

// Puts the chroma of two pixels on both, as the plain code's row decoders do.
__m128i spread_chroma(const uint8_t* four_bytes) {
    int bytes = 0;
    std::memcpy(&bytes, four_bytes, sizeof(bytes));
    return _mm_shuffle_epi8(_mm_cvtsi32_si128(bytes), _mm_setr_epi8(0, 0, 1, 1, 2, 2, 3, 3, -1, -1, -1, -1, -1, -1, -1, -1));
}

} // namespace

bool cpu_has_avx2() {
    static const bool has = [] {
        int regs[4]{};
        __cpuid(regs, 0);
        if (regs[0] < 7) return false;
        __cpuid(regs, 1);
        constexpr int kOsSaveAndAvx = (1 << 27) | (1 << 28);
        if ((regs[2] & kOsSaveAndAvx) != kOsSaveAndAvx) return false;
        if ((_xgetbv(0) & 6) != 6) return false; // the OS saves the wide registers
        __cpuidex(regs, 7, 0);
        return (regs[1] & (1 << 5)) != 0;
    }();
    return has;
}

// The same taps as FieldPrep::scale_row.
bool Avx2Rows::setup(unsigned src_width, unsigned src_x0, unsigned span_w, unsigned dest_w) {
    if (src_width == src_width_ && src_x0 == src_x0_ && span_w == span_w_ && dest_w == dest_w_) return taps_ <= kMaxTaps;
    src_width_ = src_width;
    src_x0_ = src_x0;
    span_w_ = span_w;
    dest_w_ = dest_w;
    taps_ = 0;
    for (unsigned x = 0; x < dest_w; ++x) {
        const uint64_t start = static_cast<uint64_t>(x) * span_w * 256ull / dest_w;
        uint64_t end = x + 1u == dest_w ? static_cast<uint64_t>(span_w) << 8 : static_cast<uint64_t>(x + 1u) * span_w * 256ull / dest_w;
        if (end <= start) end = start + 256u;
        const unsigned i0 = std::min(static_cast<unsigned>(start >> 8), span_w - 1);
        const unsigned i1 = std::min(static_cast<unsigned>((end - 1u) >> 8), span_w - 1);
        taps_ = std::max(taps_, i1 - i0 + 1u);
    }
    if (taps_ > kMaxTaps) return false;

    first_.assign(dest_w + 8u, 0);
    for (auto& weights : weight_) weights.assign(dest_w + 8u, 0);
    for (unsigned x = 0; x < dest_w; ++x) {
        const uint64_t start = static_cast<uint64_t>(x) * span_w * 256ull / dest_w;
        uint64_t end = x + 1u == dest_w ? static_cast<uint64_t>(span_w) << 8 : static_cast<uint64_t>(x + 1u) * span_w * 256ull / dest_w;
        if (end <= start) end = start + 256u;
        const unsigned i0 = std::min(static_cast<unsigned>(start >> 8), span_w - 1);
        const unsigned i1 = std::min(static_cast<unsigned>((end - 1u) >> 8), span_w - 1);
        first_[x] = static_cast<int>(i0);
        unsigned raw[kMaxTaps]{};
        unsigned sum = 0;
        for (unsigned i = i0; i <= i1; ++i) {
            const uint64_t lo = std::max<uint64_t>(start, uint64_t(i) << 8);
            const uint64_t hi = std::min<uint64_t>(end, uint64_t(i + 1u) << 8);
            raw[i - i0] = hi > lo ? static_cast<unsigned>(hi - lo) : 0u;
            sum += raw[i - i0];
        }
        if (!sum) sum = 1;
        unsigned cumulative = 0, given = 0;
        for (unsigned k = 0; k <= i1 - i0; ++k) {
            cumulative += raw[k];
            const unsigned target = (cumulative * 256u + sum / 2u) / sum;
            weight_[k][x] = static_cast<int>(target - given);
            given = target;
        }
    }
    for (int p = 0; p < 3; ++p) {
        plane_[p].assign(src_width + kPadding, 0);
        averaged_[p].assign(span_w + kPadding, 0);
        scaled_[p].assign(dest_w + kPadding, 0);
        acc_[p].assign(span_w + kPadding, 0);
    }
    row_.assign(dest_w + kPadding, 0);
    return true;
}

void Avx2Rows::begin_row() {
    for (auto& acc : acc_) std::memset(acc.data(), 0, span_w_ * sizeof(uint16_t));
}

// Source row `y` as three planes of src_width_ samples: RGB for decoded MJPEG, otherwise
// Y, U and V. Takes care never to read past the end of a row.
void Avx2Rows::load_row(const Sample& sample, unsigned y) {
    const unsigned width = sample.width;
    const uint8_t* row = sample.data + static_cast<ptrdiff_t>(y) * sample.pitch;
    uint8_t* p0 = plane_[0].data();
    uint8_t* p1 = plane_[1].data();
    uint8_t* p2 = plane_[2].data();
    const ptrdiff_t luma_bytes = static_cast<ptrdiff_t>(sample.height) * sample.pitch;
    unsigned x = 0;
    switch (sample.pixels) {
    case SamplePixels::Rgb: {
        const __m128i r0 = _mm_setr_epi8(0, 3, 6, 9, 12, 15, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
        const __m128i r1 = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, 2, 5, 8, 11, 14, -1, -1, -1, -1, -1);
        const __m128i r2 = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 1, 4, 7, 10, 13);
        const __m128i g0 = _mm_setr_epi8(1, 4, 7, 10, 13, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
        const __m128i g1 = _mm_setr_epi8(-1, -1, -1, -1, -1, 0, 3, 6, 9, 12, 15, -1, -1, -1, -1, -1);
        const __m128i g2 = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 2, 5, 8, 11, 14);
        const __m128i b0 = _mm_setr_epi8(2, 5, 8, 11, 14, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1);
        const __m128i b1 = _mm_setr_epi8(-1, -1, -1, -1, -1, 1, 4, 7, 10, 13, -1, -1, -1, -1, -1, -1);
        const __m128i b2 = _mm_setr_epi8(-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 0, 3, 6, 9, 12, 15);
        for (; x + 16 <= width; x += 16) {
            const __m128i a = _mm_loadu_si128(as_m128(row + x * 3));
            const __m128i b = _mm_loadu_si128(as_m128(row + x * 3 + 16));
            const __m128i c = _mm_loadu_si128(as_m128(row + x * 3 + 32));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(p0 + x), _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(a, r0), _mm_shuffle_epi8(b, r1)), _mm_shuffle_epi8(c, r2)));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(p1 + x), _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(a, g0), _mm_shuffle_epi8(b, g1)), _mm_shuffle_epi8(c, g2)));
            _mm_storeu_si128(reinterpret_cast<__m128i*>(p2 + x), _mm_or_si128(_mm_or_si128(_mm_shuffle_epi8(a, b0), _mm_shuffle_epi8(b, b1)), _mm_shuffle_epi8(c, b2)));
        }
        for (; x < width; ++x) {
            p0[x] = row[x * 3];
            p1[x] = row[x * 3 + 1];
            p2[x] = row[x * 3 + 2];
        }
        break;
    }
    case SamplePixels::Yuy2:
    case SamplePixels::Uyvy: {
        const bool yuy2 = sample.pixels == SamplePixels::Yuy2;
        const __m128i ys = yuy2 ? _mm_setr_epi8(0, 2, 4, 6, 8, 10, 12, 14, -1, -1, -1, -1, -1, -1, -1, -1)
                                : _mm_setr_epi8(1, 3, 5, 7, 9, 11, 13, 15, -1, -1, -1, -1, -1, -1, -1, -1);
        const __m128i us = yuy2 ? _mm_setr_epi8(1, 1, 5, 5, 9, 9, 13, 13, -1, -1, -1, -1, -1, -1, -1, -1)
                                : _mm_setr_epi8(0, 0, 4, 4, 8, 8, 12, 12, -1, -1, -1, -1, -1, -1, -1, -1);
        const __m128i vs = yuy2 ? _mm_setr_epi8(3, 3, 7, 7, 11, 11, 15, 15, -1, -1, -1, -1, -1, -1, -1, -1)
                                : _mm_setr_epi8(2, 2, 6, 6, 10, 10, 14, 14, -1, -1, -1, -1, -1, -1, -1, -1);
        for (; x + 8 <= width; x += 8) {
            const __m128i in = _mm_loadu_si128(as_m128(row + x * 2));
            store8(p0 + x, _mm_shuffle_epi8(in, ys));
            store8(p1 + x, _mm_shuffle_epi8(in, us));
            store8(p2 + x, _mm_shuffle_epi8(in, vs));
        }
        for (; x < width; x += 2) {
            const uint8_t* in = row + x * 2;
            p0[x] = in[yuy2 ? 0 : 1];
            p0[x + 1] = in[yuy2 ? 2 : 3];
            p1[x] = p1[x + 1] = in[yuy2 ? 1 : 0];
            p2[x] = p2[x + 1] = in[yuy2 ? 3 : 2];
        }
        break;
    }
    case SamplePixels::Nv12: {
        const uint8_t* uv = sample.data + luma_bytes + static_cast<ptrdiff_t>(y / 2u) * sample.pitch;
        std::memcpy(p0, row, width);
        const __m128i us = _mm_setr_epi8(0, 0, 2, 2, 4, 4, 6, 6, -1, -1, -1, -1, -1, -1, -1, -1);
        const __m128i vs = _mm_setr_epi8(1, 1, 3, 3, 5, 5, 7, 7, -1, -1, -1, -1, -1, -1, -1, -1);
        for (; x + 8 <= width; x += 8) {
            const __m128i in = _mm_loadl_epi64(as_m128(uv + x));
            store8(p1 + x, _mm_shuffle_epi8(in, us));
            store8(p2 + x, _mm_shuffle_epi8(in, vs));
        }
        for (; x < width; x += 2) {
            p1[x] = p1[x + 1] = uv[x];
            p2[x] = p2[x + 1] = uv[x + 1];
        }
        break;
    }
    case SamplePixels::I420: {
        const long chroma_pitch = sample.pitch / 2;
        const uint8_t* u_plane = sample.data + luma_bytes;
        const ptrdiff_t chroma_row = static_cast<ptrdiff_t>(y / 2u) * chroma_pitch;
        const uint8_t* u = u_plane + chroma_row;
        const uint8_t* v = u_plane + static_cast<ptrdiff_t>(sample.height / 2u) * chroma_pitch + chroma_row;
        std::memcpy(p0, row, width);
        for (; x + 8 <= width; x += 8) {
            store8(p1 + x, spread_chroma(u + x / 2));
            store8(p2 + x, spread_chroma(v + x / 2));
        }
        for (; x < width; x += 2) {
            p1[x] = p1[x + 1] = u[x / 2];
            p2[x] = p2[x + 1] = v[x / 2];
        }
        break;
    }
    }
}

void Avx2Rows::add_row(const Sample& sample, unsigned y, unsigned weight) {
    load_row(sample, y);
    const __m256i mul = _mm256_set1_epi16(static_cast<short>(weight));
    for (int p = 0; p < 3; ++p) {
        const uint8_t* src = plane_[p].data() + src_x0_;
        uint16_t* acc = acc_[p].data();
        for (unsigned x = 0; x < span_w_; x += 16) {
            const __m256i value = _mm256_cvtepu8_epi16(_mm_loadu_si128(as_m128(src + x)));
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(acc + x),
                                _mm256_add_epi16(_mm256_loadu_si256(as_m256(acc + x)), _mm256_mullo_epi16(value, mul)));
        }
    }
    _mm256_zeroupper();
}

void Avx2Rows::finish_row(bool rgb, uint16_t* out) {
    const __m256i round16 = _mm256_set1_epi16(128);
    const __m256i round32 = _mm256_set1_epi32(128);
    const __m256i low_byte = _mm256_set1_epi32(0xFF);
    for (int p = 0; p < 3; ++p) {
        const uint16_t* acc = acc_[p].data();
        uint8_t* averaged = averaged_[p].data();
        for (unsigned x = 0; x < span_w_; x += 32) {
            const __m256i lo = _mm256_srli_epi16(_mm256_add_epi16(_mm256_loadu_si256(as_m256(acc + x)), round16), 8);
            const __m256i hi = _mm256_srli_epi16(_mm256_add_epi16(_mm256_loadu_si256(as_m256(acc + x + 16)), round16), 8);
            _mm256_storeu_si256(reinterpret_cast<__m256i*>(averaged + x), _mm256_permute4x64_epi64(_mm256_packus_epi16(lo, hi), 0xD8));
        }
        uint8_t* scaled = scaled_[p].data();
        for (unsigned x = 0; x < dest_w_; x += 8) {
            const __m256i first = _mm256_loadu_si256(as_m256(first_.data() + x));
            __m256i sum = round32;
            for (unsigned group = 0; group < taps_; group += 4) {
                const __m256i index = _mm256_add_epi32(first, _mm256_set1_epi32(static_cast<int>(group)));
                const __m256i bytes = _mm256_i32gather_epi32(reinterpret_cast<const int*>(averaged), index, 1);
                for (unsigned k = 0; k < 4 && group + k < taps_; ++k) {
                    const __m256i taps = _mm256_loadu_si256(as_m256(weight_[group + k].data() + x));
                    const __m256i sample_byte = _mm256_and_si256(_mm256_srli_epi32(bytes, static_cast<int>(k * 8)), low_byte);
                    sum = _mm256_add_epi32(sum, _mm256_mullo_epi32(sample_byte, taps));
                }
            }
            const __m256i words = _mm256_permute4x64_epi64(_mm256_packus_epi32(_mm256_srli_epi32(sum, 8), _mm256_setzero_si256()), 0xD8);
            store8(scaled + x, _mm_packus_epi16(_mm256_castsi256_si128(words), _mm_setzero_si128()));
        }
    }

    const __m256i zero = _mm256_setzero_si256();
    const __m256i top = _mm256_set1_epi32(255);
    const auto clip = [&](__m256i value) { return _mm256_min_epi32(_mm256_max_epi32(_mm256_srai_epi32(value, 8), zero), top); };
    const auto widen = [](const uint8_t* p) { return _mm256_cvtepu8_epi32(_mm_loadl_epi64(as_m128(p))); };
    for (unsigned x = 0; x < dest_w_; x += 8) {
        __m256i r = widen(scaled_[0].data() + x);
        __m256i g = widen(scaled_[1].data() + x);
        __m256i b = widen(scaled_[2].data() + x);
        if (!rgb) {
            // BT.601 limited range, as in yuv_to_565().
            const __m256i luma = _mm256_mullo_epi32(_mm256_max_epi32(zero, _mm256_sub_epi32(r, _mm256_set1_epi32(16))), _mm256_set1_epi32(298));
            const __m256i c = _mm256_add_epi32(luma, round32);
            const __m256i u = _mm256_sub_epi32(g, round32);
            const __m256i v = _mm256_sub_epi32(b, round32);
            r = clip(_mm256_add_epi32(c, _mm256_mullo_epi32(v, _mm256_set1_epi32(409))));
            g = clip(_mm256_sub_epi32(_mm256_sub_epi32(c, _mm256_mullo_epi32(u, _mm256_set1_epi32(100))), _mm256_mullo_epi32(v, _mm256_set1_epi32(208))));
            b = clip(_mm256_add_epi32(c, _mm256_mullo_epi32(u, _mm256_set1_epi32(516))));
        }
        const __m256i pixel = _mm256_or_si256(_mm256_or_si256(_mm256_slli_epi32(_mm256_srli_epi32(r, 3), 11), _mm256_slli_epi32(_mm256_srli_epi32(g, 2), 5)), _mm256_srli_epi32(b, 3));
        const __m256i words = _mm256_permute4x64_epi64(_mm256_packus_epi32(pixel, zero), 0xD8);
        _mm_storeu_si128(reinterpret_cast<__m128i*>(row_.data() + x), _mm256_castsi256_si128(words));
    }
    std::memcpy(out, row_.data(), dest_w_ * sizeof(uint16_t));
    _mm256_zeroupper();
}

} // namespace groovy
