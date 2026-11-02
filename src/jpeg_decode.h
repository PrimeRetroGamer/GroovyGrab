#pragma once

#include <turbojpeg.h>

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

// Synchronous libjpeg-turbo decode. One handle per thread. The capture
// callback never calls this; the prep thread and the idle preview path do.
namespace jpeg_decode {

class Decoder {
public:
    Decoder() : handle_(tjInitDecompress()) {
        if (!handle_) {
            throw std::runtime_error(std::string("tjInitDecompress failed: ") +
                                     tjGetErrorStr2(nullptr));
        }
    }

    ~Decoder() {
        if (handle_) tjDestroy(handle_);
    }

    Decoder(const Decoder&) = delete;
    Decoder& operator=(const Decoder&) = delete;

    // RGB24, tightly packed. width and height must match the JPEG.
    bool decompress(const uint8_t* jpeg, size_t size, int width, int height,
                    std::vector<uint8_t>& rgb, std::string* error) {
        if (!jpeg || size < 4 || width < 2 || height < 1) {
            if (error) *error = "empty JPEG";
            return false;
        }
        rgb.resize(static_cast<size_t>(width) * static_cast<size_t>(height) * 3u);
        const int rc = tjDecompress2(
            handle_, jpeg, static_cast<unsigned long>(size), rgb.data(), width,
            0, height, TJPF_RGB, TJFLAG_FASTDCT | TJFLAG_FASTUPSAMPLE);
        if (rc != 0) {
            if (error) *error = tjGetErrorStr2(handle_);
            rgb.clear();
            return false;
        }
        return true;
    }

private:
    tjhandle handle_ = nullptr;
};

}  // namespace jpeg_decode
