// encoder/include/encoder_nvidia.hpp
#pragma once

#include "encoder.hpp"

#include <cstddef>
#include <cstdint>
#include <vector>

#include <cuda_runtime_api.h>
#include <d3d11.h>
#include <nvjpeg.h>
#include <wrl/client.h>

namespace vkit {

    // nvJPEG + CUDA backend. Texture input is read in place through CUDA/D3D11
    // interop, so the full image is never copied back to the CPU.
    class NvidiaJpegEncoder {
    private:
        JpegEncoderDesc desc_{};
        nvjpegHandle_t handle_ = nullptr;               // nvJPEG library context
        nvjpegEncoderState_t encode_state_ = nullptr;   // reusable per-encoder JPEG state
        nvjpegEncoderParams_t encode_params_ = nullptr; // quality and sampling settings
        cudaStream_t stream_ = nullptr;                 // CUDA command stream
        unsigned char* device_rgb_ = nullptr;           // GPU-side interleaved RGB buffer
        cudaGraphicsResource_t graphics_resource_ = nullptr; // CUDA handle for the texture
        // The texture behind graphics_resource_. Holding a reference keeps it alive
        // while CUDA has it registered, and stops a new texture created at the same
        // address from being mistaken for the old one.
        Microsoft::WRL::ComPtr<ID3D11Texture2D> registered_texture_;
        std::vector<uint8_t> host_rgb_;                 // CPU staging buffer, reused per frame
        bool initialized_ = false;
        bool texture_mode_ = false;

        bool encode_device_rgb(std::vector<uint8_t>& jpeg_output);

    public:
        NvidiaJpegEncoder() = default;
        ~NvidiaJpegEncoder();

        NvidiaJpegEncoder(const NvidiaJpegEncoder&) = delete;
        NvidiaJpegEncoder& operator=(const NvidiaJpegEncoder&) = delete;

        // CPU-input mode. Kept for the standalone encoder test and fallback uses.
        bool init(const JpegEncoderDesc& desc);

        // GPU-input mode. d3d_device must be the same D3D11 device that owns the
        // texture passed to encode_texture().
        bool init(ID3D11Device* d3d_device, const JpegEncoderDesc& desc);

        bool encode_bgra(const uint8_t* pixels,
                         size_t row_pitch,
                         std::vector<uint8_t>& jpeg_output);

        // Requires the D3D11 init() overload.
        bool encode_texture(ID3D11Texture2D* texture,
                            std::vector<uint8_t>& jpeg_output);

        void cleanup();

        JpegEncoderDesc get_desc() const { return desc_; }
    };

} // namespace vkit
