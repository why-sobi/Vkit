// encoder/include/encoder.hpp

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace vkit {

struct JpegEncoderDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    int quality = 75;
};

class JpegEncoder {
public:
    JpegEncoder() = default;
    ~JpegEncoder();

    JpegEncoder(const JpegEncoder&) = delete;
    JpegEncoder& operator=(const JpegEncoder&) = delete;

    bool init(const JpegEncoderDesc& desc);

    bool encode_bgra(
        const uint8_t* pixels,
        size_t row_pitch,
        std::vector<uint8_t>& jpeg_output
    );

    void cleanup();

private:
    JpegEncoderDesc desc_{};
    void* handle_ = nullptr;
    void* encode_state_ = nullptr;
    void* encode_params_ = nullptr;
    void* stream_ = nullptr;
    void* device_rgb_ = nullptr;
};

}