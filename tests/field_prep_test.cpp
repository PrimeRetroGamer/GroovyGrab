// Plain and AVX2 conversion must give the same bytes for every format, size, mode and view.
// Run after changing either. Frames are the exact size of a capture sample, so building
// with /fsanitize=address also catches any read past the end of one.
#include "field_prep.h"
#include "field_prep_avx2.h"
#include "lz4.h"

#include <cstdio>
#include <cstring>
#include <random>
#include <vector>

using namespace groovy;

namespace {

struct Format {
    const char* name;
    SamplePixels pixels;
};

std::vector<uint8_t> make_frame(SamplePixels pixels, unsigned width, unsigned height, long& pitch, int kind, std::mt19937& rng) {
    size_t bytes = 0;
    switch (pixels) {
    case SamplePixels::Rgb:
        pitch = static_cast<long>(width) * 3;
        bytes = static_cast<size_t>(pitch) * height;
        break;
    case SamplePixels::Yuy2:
    case SamplePixels::Uyvy:
        pitch = static_cast<long>(width) * 2;
        bytes = static_cast<size_t>(pitch) * height;
        break;
    case SamplePixels::Nv12:
    case SamplePixels::I420:
        pitch = static_cast<long>(width);
        bytes = static_cast<size_t>(pitch) * (height + height / 2u);
        break;
    }
    std::vector<uint8_t> frame(bytes);
    for (size_t i = 0; i < bytes; ++i) {
        switch (kind) {
        case 0: frame[i] = static_cast<uint8_t>(rng()); break; // noise
        case 1: frame[i] = 0; break;
        case 2: frame[i] = 255; break;
        case 3: frame[i] = (rng() & 1) ? 255 : 0; break; // the colour conversion's clamps
        default: frame[i] = static_cast<uint8_t>((i * 7) >> 3); break; // a gradient
        }
    }
    return frame;
}

} // namespace

int main() {
    if (!cpu_has_avx2()) {
        std::puts("This CPU has no AVX2: nothing to compare");
        return 0;
    }
    const Format formats[] = {{"YUY2", SamplePixels::Yuy2}, {"UYVY", SamplePixels::Uyvy}, {"NV12", SamplePixels::Nv12},
                              {"I420", SamplePixels::I420}, {"RGB", SamplePixels::Rgb}};
    const unsigned sizes[][2] = {{1280, 720}, {1920, 1080}, {3840, 2160}, {720, 480}, {720, 576}, {640, 480}, {854, 480},
                                 {1366, 768}, {1024, 768},  {800, 600},   {320, 240}, {160, 120}, {2, 2},     {6, 4},
                                 {1282, 722}, {7680, 4320}}; // the last takes more taps than AVX2 handles
    const VideoMode modes[] = {VideoMode::i480, VideoMode::p240, VideoMode::i576, VideoMode::p288};
    const ViewSettings views[] = {{false, 0, 0},    {true, 0, 0},     {false, 10, 0},  {false, 0, 10},  {false, -10, -10},
                                  {false, 25, 25},  {true, -25, 13},  {false, -7, 3},  {false, 49, -49}};
    std::mt19937 rng(12345);
    FieldPrep plain, fast;
    plain.set_avx2_allowed(false);
    PreparedField a, b;
    std::vector<uint8_t> pixels_a(720 * 288 * 2), pixels_b(720 * 288 * 2);
    uint64_t cases = 0, wrong = 0;
    for (const Format& format : formats) {
        const uint64_t cases_before = cases, wrong_before = wrong;
        for (const auto& size : sizes) {
            for (int kind = 0; kind < 5; ++kind) {
                long pitch = 0;
                const std::vector<uint8_t> frame = make_frame(format.pixels, size[0], size[1], pitch, kind, rng);
                Sample sample;
                sample.data = frame.data();
                sample.pitch = pitch;
                sample.width = size[0];
                sample.height = size[1];
                sample.pixels = format.pixels;
                for (const VideoMode mode : modes) {
                    for (const ViewSettings& view : views) {
                        for (uint8_t parity = 0; parity < (is_interlaced(mode) ? 2 : 1); ++parity) {
                            const bool ok_a = plain.prepare_field(sample, mode, view, parity, false, a);
                            const bool ok_b = fast.prepare_field(sample, mode, view, parity, false, b);
                            const int raw = 720 * static_cast<int>(output_height(mode)) * 2;
                            const int size_a = ok_a ? LZ4_decompress_safe(a.data[parity].data(), reinterpret_cast<char*>(pixels_a.data()), a.size[parity], raw) : 0;
                            const int size_b = ok_b ? LZ4_decompress_safe(b.data[parity].data(), reinterpret_cast<char*>(pixels_b.data()), b.size[parity], raw) : 0;
                            ++cases;
                            if (ok_a != ok_b || size_a != size_b || std::memcmp(pixels_a.data(), pixels_b.data(), static_cast<size_t>(size_a > 0 ? size_a : 0)) != 0) {
                                if (wrong < 12) {
                                    std::printf("DIFFERENT: %s %ux%u, data %d, mode %d, letterbox %d, crop %d,%d, parity %d\n", format.name, size[0], size[1], kind,
                                                static_cast<int>(mode), view.letterbox ? 1 : 0, view.crop_h, view.crop_v, parity);
                                }
                                ++wrong;
                            }
                        }
                    }
                }
            }
        }
        std::printf("%-5s %llu cases, %llu different\n", format.name, static_cast<unsigned long long>(cases - cases_before),
                    static_cast<unsigned long long>(wrong - wrong_before));
    }
    std::printf("Total %llu cases, %llu different\n", static_cast<unsigned long long>(cases), static_cast<unsigned long long>(wrong));
    return wrong ? 1 : 0;
}
