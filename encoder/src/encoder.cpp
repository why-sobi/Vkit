#include "encoder.hpp"

#include <iostream>

// ============================================================================
// NVIDIA backend (nvJPEG + CUDA)
// ============================================================================

#ifdef VKIT_HAS_NVJPEG

#include <cuda_runtime_api.h>
#include <cuda_d3d11_interop.h>
#include <nvjpeg.h>
#include <wrl/client.h>

// Implemented in encoder_cuda.cu. A .cpp file is compiled as ordinary C++ and
// cannot contain CUDA kernel launches, so the small GPU conversion helper lives
// in a CUDA source file and is exposed through this C-linkage function.
extern "C" bool vkit_convert_bgra_array_to_rgb (
    cudaArray_t source,
    unsigned char* destination,
    uint32_t width,
    uint32_t height,
    cudaStream_t stream
);

namespace vkit {

namespace {

bool check(cudaError_t status, const char* operation) {
    if (status == cudaSuccess) return true;
    std::cerr << "[JpegEncoder] " << operation << " failed: "
              << cudaGetErrorString(status) << '\n';
    return false;
}

bool check(nvjpegStatus_t status, const char* operation) {
    if (status == NVJPEG_STATUS_SUCCESS) return true;
    std::cerr << "[JpegEncoder] " << operation
              << " failed with nvJPEG status " << static_cast<int>(status) << '\n';
    return false;
}

} // namespace

struct JpegEncoder::NvjpegBackend {
    JpegEncoderDesc desc{};
    nvjpegHandle_t handle = nullptr;               // nvJPEG library context
    nvjpegEncoderState_t encode_state = nullptr;   // reusable per-encoder JPEG state
    nvjpegEncoderParams_t encode_params = nullptr; // quality and sampling settings
    cudaStream_t stream = nullptr;                 // CUDA command stream
    unsigned char* device_rgb = nullptr;           // GPU-side interleaved RGB buffer
    cudaGraphicsResource_t graphics_resource = nullptr; // CUDA handle for the texture
    // The texture behind graphics_resource. Holding a reference keeps it alive
    // while CUDA has it registered, and stops a new texture created at the same
    // address from being mistaken for the old one.
    Microsoft::WRL::ComPtr<ID3D11Texture2D> registered_texture;
    std::vector<uint8_t> host_rgb;                 // CPU staging buffer, reused per frame
    bool texture_mode = false;

    ~NvjpegBackend() {
        // Unregister before releasing the encoder's other CUDA resources.
        if (graphics_resource) cudaGraphicsUnregisterResource(graphics_resource);
        if (device_rgb) cudaFree(device_rgb);
        if (stream) cudaStreamDestroy(stream);
        if (encode_params) nvjpegEncoderParamsDestroy(encode_params);
        if (encode_state) nvjpegEncoderStateDestroy(encode_state);
        if (handle) nvjpegDestroy(handle);
    }

    // On failure, partially created resources are released by the destructor.
    bool init(ID3D11Device* d3d_device, const JpegEncoderDesc& encoder_desc) {
        desc = encoder_desc;
        texture_mode = d3d_device != nullptr;

        if (d3d_device) {
            // D3D11 and CUDA must use the same physical NVIDIA adapter. Selecting
            // CUDA device 0 blindly would be wrong on multi-GPU systems.
            unsigned int device_count = 0;
            int device = -1;
            if (!check(cudaD3D11GetDevices(&device_count, &device, 1, d3d_device,
                                           cudaD3D11DeviceListAll),
                       "cudaD3D11GetDevices") ||
                device_count == 0 ||
                !check(cudaSetDevice(device), "cudaSetDevice")) {
                return false;
            }
        }

        if (!check(nvjpegCreateSimple(&handle), "nvjpegCreateSimple") ||
            !check(nvjpegEncoderStateCreate(handle, &encode_state, nullptr),
                   "nvjpegEncoderStateCreate") ||
            !check(nvjpegEncoderParamsCreate(handle, &encode_params, nullptr),
                   "nvjpegEncoderParamsCreate") ||
            !check(nvjpegEncoderParamsSetQuality(encode_params, desc.quality, nullptr),
                   "nvjpegEncoderParamsSetQuality") ||
            !check(nvjpegEncoderParamsSetSamplingFactors(
                       encode_params, NVJPEG_CSS_444, nullptr),
                   "nvjpegEncoderParamsSetSamplingFactors") ||
            !check(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking),
                   "cudaStreamCreateWithFlags")) {
            return false;
        }

        // nvJPEG receives RGBI (interleaved RGB), so reserve 3 bytes per pixel on
        // the GPU. This buffer is reused for every frame.
        const size_t rgb_size = static_cast<size_t>(desc.width) * desc.height * 3;
        return check(cudaMalloc(reinterpret_cast<void**>(&device_rgb), rgb_size),
                     "cudaMalloc");
    }

    bool encode_bgra(
        const uint8_t* pixels,
        size_t row_pitch,
        std::vector<uint8_t>& jpeg_output) {
        // Convert BGRA to packed RGB on the CPU, then upload it for nvJPEG.
        const size_t rgb_pitch = static_cast<size_t>(desc.width) * 3;
        host_rgb.resize(rgb_pitch * desc.height);
        for (uint32_t y = 0; y < desc.height; ++y) {
            const uint8_t* src = pixels + y * row_pitch;
            uint8_t* dst = host_rgb.data() + y * rgb_pitch;
            for (uint32_t x = 0; x < desc.width; ++x, src += 4, dst += 3) {
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
            }
        }

        if (!check(cudaMemcpyAsync(device_rgb, host_rgb.data(), host_rgb.size(),
                                   cudaMemcpyHostToDevice, stream),
                   "cudaMemcpyAsync")) {
            return false;
        }

        return encode_device_rgb(jpeg_output);
    }

    bool encode_texture(
        ID3D11Texture2D* texture,
        std::vector<uint8_t>& jpeg_output) {
        if (!texture_mode) {
            std::cerr << "[JpegEncoder] Texture encoding is not initialized.\n";
            return false;
        }

        // Registration connects this D3D11 resource to CUDA. It does not copy
        // the image; it lets CUDA access the texture in place. It is done once
        // per texture, and redone if the caller switches to a new texture (for
        // example after the scaler recreates its output on resize).
        if (texture != registered_texture.Get()) {
            if (graphics_resource) {
                cudaGraphicsUnregisterResource(graphics_resource);
                graphics_resource = nullptr;
                registered_texture.Reset();
            }

            if (!check(cudaGraphicsD3D11RegisterResource(
                           &graphics_resource, texture, cudaGraphicsRegisterFlagsNone),
                       "cudaGraphicsD3D11RegisterResource")) {
                graphics_resource = nullptr;
                return false;
            }
            registered_texture = texture;
        }

        if (!check(cudaGraphicsMapResources(1, &graphics_resource, stream),
                   "cudaGraphicsMapResources")) {
            return false;
        }

        // A mapped D3D11 texture is exposed as a CUDA array. The CUDA kernel reads
        // that array and writes a linear RGB buffer that nvJPEG can consume.
        cudaArray_t source = nullptr;
        bool success = check(cudaGraphicsSubResourceGetMappedArray(
                                 &source, graphics_resource, 0, 0),
                             "cudaGraphicsSubResourceGetMappedArray");
        if (success) {
            success = vkit_convert_bgra_array_to_rgb(
                source, device_rgb, desc.width, desc.height, stream);
        }

        // The graphics resource must be unmapped before D3D11 uses it again.
        // Unmapping on the same stream orders it after the conversion kernel, so
        // no CPU-side wait is needed here.
        const cudaError_t unmap_status =
            cudaGraphicsUnmapResources(1, &graphics_resource, stream);
        if (!success || !check(unmap_status, "cudaGraphicsUnmapResources")) {
            return false;
        }

        return encode_device_rgb(jpeg_output);
    }

private:
    // Compresses device_rgb on the GPU. Only the compressed bitstream is copied
    // into jpeg_output; the full image is never read back to the CPU. All work
    // is queued on one stream, so the CPU waits only once, before the copy out.
    bool encode_device_rgb(std::vector<uint8_t>& jpeg_output) {
        nvjpegImage_t image{};
        image.channel[0] = device_rgb;
        image.pitch[0] = static_cast<size_t>(desc.width) * 3;

        if (!check(nvjpegEncodeImage(handle, encode_state, encode_params, &image,
                                     NVJPEG_INPUT_RGBI, desc.width, desc.height, stream),
                   "nvjpegEncodeImage")) {
            return false;
        }

        size_t output_size = 0;
        if (!check(nvjpegEncodeRetrieveBitstream(
                       handle, encode_state, nullptr, &output_size, stream),
                   "nvjpegEncodeRetrieveBitstream(size)")) {
            return false;
        }

        // The single CPU wait: conversion and encoding must be done before the
        // bitstream is copied out.
        if (!check(cudaStreamSynchronize(stream), "cudaStreamSynchronize")) {
            return false;
        }

        jpeg_output.resize(output_size);
        if (!check(nvjpegEncodeRetrieveBitstream(
                       handle, encode_state, jpeg_output.data(), &output_size, stream),
                   "nvjpegEncodeRetrieveBitstream(data)")) {
            jpeg_output.clear();
            return false;
        }
        jpeg_output.resize(output_size);
        return true;
    }
};

} // namespace vkit

#else // !VKIT_HAS_NVJPEG

namespace vkit {

// Stub used when the CUDA Toolkit is not installed, so JpegEncoder compiles
// without nvJPEG. init() always fails, so the encode functions are never reached.
struct JpegEncoder::NvjpegBackend {
    bool init(ID3D11Device*, const JpegEncoderDesc&) {
        std::cerr << "[JpegEncoder] nvJPEG is unavailable. Install the CUDA Toolkit.\n";
        return false;
    }
    bool encode_bgra(const uint8_t*, size_t, std::vector<uint8_t>&) { return false; }
    bool encode_texture(ID3D11Texture2D*, std::vector<uint8_t>&) { return false; }
};

} // namespace vkit

#endif // VKIT_HAS_NVJPEG

// ============================================================================
// JpegEncoder: validates input and forwards to the active backend
// ============================================================================

namespace vkit {

JpegEncoder::JpegEncoder() = default;

JpegEncoder::~JpegEncoder() = default;

bool JpegEncoder::init(const JpegEncoderDesc& desc) {
    return init(nullptr, desc);
}

bool JpegEncoder::init(ID3D11Device* d3d_device, const JpegEncoderDesc& desc) {
    cleanup();

    if (desc.width == 0 || desc.height == 0 ||
        desc.quality < 1 || desc.quality > 100) {
        std::cerr << "[JpegEncoder] Width, height, and quality must be valid.\n";
        return false;
    }

    nvjpeg_ = std::make_unique<NvjpegBackend>();
    if (!nvjpeg_->init(d3d_device, desc)) {
        nvjpeg_.reset();
        return false;
    }

    desc_ = desc;
    return true;
}

bool JpegEncoder::encode_bgra(
    const uint8_t* pixels,
    size_t row_pitch,
    std::vector<uint8_t>& jpeg_output) {
    jpeg_output.clear();

    if (!nvjpeg_) {
        std::cerr << "[JpegEncoder] Encoder is not initialized.\n";
        return false;
    }
    if (!pixels || row_pitch < static_cast<size_t>(desc_.width) * 4) {
        std::cerr << "[JpegEncoder] Invalid BGRA input.\n";
        return false;
    }

    return nvjpeg_->encode_bgra(pixels, row_pitch, jpeg_output);
}

bool JpegEncoder::encode_texture(
    ID3D11Texture2D* texture,
    std::vector<uint8_t>& jpeg_output) {
    jpeg_output.clear();

    if (!nvjpeg_ || !texture) {
        std::cerr << "[JpegEncoder] Texture encoding is not initialized.\n";
        return false;
    }

    return nvjpeg_->encode_texture(texture, jpeg_output);
}

void JpegEncoder::cleanup() {
    nvjpeg_.reset();
    desc_ = {};
}

} // namespace vkit
