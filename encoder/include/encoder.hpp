// encoder/include/encoder.hpp
#pragma once

#include <cstdint>

namespace vkit {

    struct JpegEncoderDesc {
        uint32_t width = 0;
        uint32_t height = 0;
        int quality = 75;
    };

    // Every backend exposes the same interface:
    //
    //   bool init(const JpegEncoderDesc& desc);                       // CPU input
    //   bool init(ID3D11Device* d3d_device, const JpegEncoderDesc&);  // GPU input
    //   bool encode_bgra(const uint8_t* pixels, size_t row_pitch,
    //                    std::vector<uint8_t>& jpeg_output);
    //   bool encode_texture(ID3D11Texture2D* texture,
    //                       std::vector<uint8_t>& jpeg_output);
    //   void cleanup();
    //
    // The compressed JPEG bytes are returned in CPU memory so they can be sent
    // over a normal socket to the ESP32.

    // Forward declare the concrete implementation class so the alias knows it exists
    #if defined(VKIT_USE_INTEL_VPL)
        class IntelJpegEncoder;
    #elif defined(VKIT_USE_NVIDIA_NVJPEG)
        class NvidiaJpegEncoder;
    #endif

} // namespace vkit

// Backend selector alias (the backend is chosen by VKIT_ENCODER_BACKEND in CMake)
#if defined(VKIT_USE_INTEL_VPL)
    #include "encoder_intel.hpp"
    namespace vkit {
        using JpegEncoder = IntelJpegEncoder;
    }
#elif defined(VKIT_USE_NVIDIA_NVJPEG)
    #include "encoder_nvidia.hpp"
    namespace vkit {
        using JpegEncoder = NvidiaJpegEncoder;
    }
#else
    #error "No encoder backend selected. Set VKIT_ENCODER_BACKEND to INTEL or NVIDIA."
#endif
