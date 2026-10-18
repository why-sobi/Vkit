// encoder/include/encoder.hpp
#pragma once

#include <cstdint>

namespace vkit {

    // Pixel layout of the frames handed to the encoder. NV12 is already 4:2:0,
    // so the JPEG gets subsampled chroma (much smaller); BGRA32 encodes 4:4:4.
    enum class JpegInputFormat {
        BGRA32,
        NV12      // Intel backend only; width and height must be even
    };

    struct JpegEncoderDesc {
        uint32_t width = 0;
        uint32_t height = 0;
        int quality = 75;
        JpegInputFormat input_format = JpegInputFormat::BGRA32;
    };

    // Every backend exposes the same interface:
    //
    //   bool init(const JpegEncoderDesc& desc);                       // CPU input
    //   bool init(ID3D11Device* d3d_device, const JpegEncoderDesc&);  // GPU input
    //   std::span<const uint8_t> encode_bgra(const uint8_t* pixels, size_t row_pitch);  // BGRA32 input only
    //   std::span<const uint8_t> encode_texture(ID3D11Texture2D* texture);               // texture format must match input_format
    //   void cleanup();
    //   JpegEncoderDesc get_desc() const;
    //   bool uses_video_memory() const;   // true when frames stay on the GPU
    //
    // The encode functions return a view of the finished JPEG in system RAM,
    // ready to hand to send() for the ESP32 stream. The view points into the
    // encoder's own output buffer (no extra copy), so it is only valid until the
    // next encode, init() or cleanup() call. An empty span means failure.

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
