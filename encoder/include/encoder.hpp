// encoder/include/encoder.hpp

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

struct ID3D11Device;
struct ID3D11Texture2D;

namespace vkit {

struct JpegEncoderDesc {
    uint32_t width = 0;
    uint32_t height = 0;
    int quality = 75;
};

// Encodes either CPU BGRA pixels or a D3D11 BGRA texture into one JPEG image.
// The compressed JPEG bytes are returned in CPU memory so they can be sent over
// a normal socket to the ESP32.
class JpegEncoder {
public:
    JpegEncoder();
    ~JpegEncoder();

    JpegEncoder(const JpegEncoder&) = delete;
    JpegEncoder& operator=(const JpegEncoder&) = delete;

    // CPU-input mode. Kept for the standalone encoder test and fallback uses.
    bool init(const JpegEncoderDesc& desc);

    // GPU-input mode. d3d_device must be the same D3D11 device that owns the
    // texture passed to encode_texture().
    bool init(ID3D11Device* d3d_device, const JpegEncoderDesc& desc);

    bool encode_bgra(
        const uint8_t* pixels,
        size_t row_pitch,
        std::vector<uint8_t>& jpeg_output
    );

    // Encodes a D3D11 BGRA texture on the GPU without reading the full image
    // back to the CPU. Requires the D3D11 init() overload.
    bool encode_texture(
        ID3D11Texture2D* texture,
        std::vector<uint8_t>& jpeg_output
    );

    void cleanup();

private:
    // Vendor backends. Each one is defined entirely in encoder.cpp inside its
    // own #ifdef section, so the vendor SDK headers never leak into this file.
    // Add new backends (e.g. AMD) next to this one.
    struct NvjpegBackend;

    JpegEncoderDesc desc_{};
    std::unique_ptr<NvjpegBackend> nvjpeg_;
};

}