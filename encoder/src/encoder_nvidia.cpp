// src/encoder_nvidia.cpp

#include "encoder_nvidia.hpp"

#include <cuda_d3d11_interop.h>
#include <iostream>

// Implemented in encoder_cuda.cu. A .cpp file is compiled as ordinary C++ and
// cannot contain CUDA kernel launches, so the small GPU conversion helper lives
// in a CUDA source file and is exposed through this C-linkage function.
extern "C" bool vkit_convert_bgra_array_to_rgb(
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
            std::cerr << "[NvidiaJpegEncoder] " << operation << " failed: "
                      << cudaGetErrorString(status) << '\n';
            return false;
        }

        bool check(nvjpegStatus_t status, const char* operation) {
            if (status == NVJPEG_STATUS_SUCCESS) return true;
            std::cerr << "[NvidiaJpegEncoder] " << operation
                      << " failed with nvJPEG status " << static_cast<int>(status) << '\n';
            return false;
        }

    } // namespace

    NvidiaJpegEncoder::~NvidiaJpegEncoder() {
        cleanup();
    }

    bool NvidiaJpegEncoder::init(const JpegEncoderDesc& desc) {
        return init(nullptr, desc);
    }

    // On failure, partially created resources are released by cleanup().
    bool NvidiaJpegEncoder::init(ID3D11Device* d3d_device, const JpegEncoderDesc& desc) {
        cleanup();

        if (desc.width == 0 || desc.height == 0 ||
            desc.quality < 1 || desc.quality > 100) {
            std::cerr << "[NvidiaJpegEncoder] Width, height, and quality must be valid.\n";
            return false;
        }
        if (desc.input_format != JpegInputFormat::BGRA32) {
            std::cerr << "[NvidiaJpegEncoder] Only BGRA32 input is supported.\n";
            return false;
        }

        desc_ = desc;
        texture_mode_ = d3d_device != nullptr;

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
                cleanup();
                return false;
            }
        }

        if (!check(nvjpegCreateSimple(&handle_), "nvjpegCreateSimple") ||
            !check(nvjpegEncoderStateCreate(handle_, &encode_state_, nullptr),
                   "nvjpegEncoderStateCreate") ||
            !check(nvjpegEncoderParamsCreate(handle_, &encode_params_, nullptr),
                   "nvjpegEncoderParamsCreate") ||
            !check(nvjpegEncoderParamsSetQuality(encode_params_, desc_.quality, nullptr),
                   "nvjpegEncoderParamsSetQuality") ||
            !check(nvjpegEncoderParamsSetSamplingFactors(
                       encode_params_, NVJPEG_CSS_444, nullptr),
                   "nvjpegEncoderParamsSetSamplingFactors") ||
            !check(cudaStreamCreateWithFlags(&stream_, cudaStreamNonBlocking),
                   "cudaStreamCreateWithFlags")) {
            cleanup();
            return false;
        }

        // nvJPEG receives RGBI (interleaved RGB), so reserve 3 bytes per pixel on
        // the GPU. This buffer is reused for every frame.
        const size_t rgb_size = static_cast<size_t>(desc_.width) * desc_.height * 3;
        if (!check(cudaMalloc(reinterpret_cast<void**>(&device_rgb_), rgb_size),
                   "cudaMalloc")) {
            cleanup();
            return false;
        }

        initialized_ = true;
        return true;
    }

    std::span<const uint8_t> NvidiaJpegEncoder::encode_bgra(const uint8_t* pixels, size_t row_pitch) {
        if (!initialized_) {
            std::cerr << "[NvidiaJpegEncoder] Encoder is not initialized.\n";
            return {};
        }
        if (!pixels || row_pitch < static_cast<size_t>(desc_.width) * 4) {
            std::cerr << "[NvidiaJpegEncoder] Invalid BGRA input.\n";
            return {};
        }

        // Convert BGRA to packed RGB on the CPU, then upload it for nvJPEG.
        const size_t rgb_pitch = static_cast<size_t>(desc_.width) * 3;
        host_rgb_.resize(rgb_pitch * desc_.height);
        for (uint32_t y = 0; y < desc_.height; ++y) {
            const uint8_t* src = pixels + y * row_pitch;
            uint8_t* dst = host_rgb_.data() + y * rgb_pitch;
            for (uint32_t x = 0; x < desc_.width; ++x, src += 4, dst += 3) {
                dst[0] = src[2];
                dst[1] = src[1];
                dst[2] = src[0];
            }
        }

        if (!check(cudaMemcpyAsync(device_rgb_, host_rgb_.data(), host_rgb_.size(),
                                   cudaMemcpyHostToDevice, stream_),
                   "cudaMemcpyAsync")) {
            return {};
        }

        return encode_device_rgb();
    }

    std::span<const uint8_t> NvidiaJpegEncoder::encode_texture(ID3D11Texture2D* texture) {
        if (!initialized_ || !texture_mode_ || !texture) {
            std::cerr << "[NvidiaJpegEncoder] Texture encoding is not initialized.\n";
            return {};
        }

        // Registration connects this D3D11 resource to CUDA. It does not copy
        // the image; it lets CUDA access the texture in place. It is done once
        // per texture, and redone if the caller switches to a new texture (for
        // example after the scaler recreates its output on resize).
        if (texture != registered_texture_.Get()) {
            if (graphics_resource_) {
                cudaGraphicsUnregisterResource(graphics_resource_);
                graphics_resource_ = nullptr;
                registered_texture_.Reset();
            }

            if (!check(cudaGraphicsD3D11RegisterResource(
                           &graphics_resource_, texture, cudaGraphicsRegisterFlagsNone),
                       "cudaGraphicsD3D11RegisterResource")) {
                graphics_resource_ = nullptr;
                return {};
            }
            registered_texture_ = texture;
        }

        if (!check(cudaGraphicsMapResources(1, &graphics_resource_, stream_),
                   "cudaGraphicsMapResources")) {
            return {};
        }

        // A mapped D3D11 texture is exposed as a CUDA array. The CUDA kernel reads
        // that array and writes a linear RGB buffer that nvJPEG can consume.
        cudaArray_t source = nullptr;
        bool success = check(cudaGraphicsSubResourceGetMappedArray(
                                 &source, graphics_resource_, 0, 0),
                             "cudaGraphicsSubResourceGetMappedArray");
        if (success) {
            success = vkit_convert_bgra_array_to_rgb(
                source, device_rgb_, desc_.width, desc_.height, stream_);
        }

        // The graphics resource must be unmapped before D3D11 uses it again.
        // Unmapping on the same stream orders it after the conversion kernel, so
        // no CPU-side wait is needed here.
        const cudaError_t unmap_status =
            cudaGraphicsUnmapResources(1, &graphics_resource_, stream_);
        if (!success || !check(unmap_status, "cudaGraphicsUnmapResources")) {
            return {};
        }

        return encode_device_rgb();
    }

    // Compresses device_rgb_ on the GPU. Only the compressed bitstream is copied
    // into jpeg_ (system RAM); the full image is never read back to the CPU.
    // jpeg_ keeps its capacity, so steady-state frames do not reallocate.
    std::span<const uint8_t> NvidiaJpegEncoder::encode_device_rgb() {
        nvjpegImage_t image{};
        image.channel[0] = device_rgb_;
        image.pitch[0] = static_cast<size_t>(desc_.width) * 3;

        if (!check(nvjpegEncodeImage(handle_, encode_state_, encode_params_, &image,
                                     NVJPEG_INPUT_RGBI, desc_.width, desc_.height, stream_),
                   "nvjpegEncodeImage")) {
            return {};
        }

        size_t output_size = 0;
        if (!check(nvjpegEncodeRetrieveBitstream(
                       handle_, encode_state_, nullptr, &output_size, stream_),
                   "nvjpegEncodeRetrieveBitstream(size)")) {
            return {};
        }

        // The single CPU wait: conversion and encoding must be done before the
        // bitstream is copied out.
        if (!check(cudaStreamSynchronize(stream_), "cudaStreamSynchronize")) {
            return {};
        }

        jpeg_.resize(output_size);
        // Synchronize again so the bytes are fully in RAM before the caller sends them.
        if (!check(nvjpegEncodeRetrieveBitstream(
                       handle_, encode_state_, jpeg_.data(), &output_size, stream_),
                   "nvjpegEncodeRetrieveBitstream(data)") ||
            !check(cudaStreamSynchronize(stream_), "cudaStreamSynchronize")) {
            return {};
        }
        return {jpeg_.data(), output_size};
    }

    bool NvidiaJpegEncoder::set_quality(int quality) {
        if (!encode_params_ || quality < 1 || quality > 100) return false;
        if (quality == desc_.quality) return true;

        if (!check(nvjpegEncoderParamsSetQuality(encode_params_, quality, stream_),
                   "nvjpegEncoderParamsSetQuality")) {
            return false;
        }

        desc_.quality = quality;
        return true;
    }

    void NvidiaJpegEncoder::cleanup() {
        // Unregister before releasing the encoder's other CUDA resources.
        if (graphics_resource_) cudaGraphicsUnregisterResource(graphics_resource_);
        if (device_rgb_) cudaFree(device_rgb_);
        if (stream_) cudaStreamDestroy(stream_);
        if (encode_params_) nvjpegEncoderParamsDestroy(encode_params_);
        if (encode_state_) nvjpegEncoderStateDestroy(encode_state_);
        if (handle_) nvjpegDestroy(handle_);

        graphics_resource_ = nullptr;
        device_rgb_ = nullptr;
        stream_ = nullptr;
        encode_params_ = nullptr;
        encode_state_ = nullptr;
        handle_ = nullptr;
        registered_texture_.Reset();
        host_rgb_.clear();
        jpeg_.clear();
        desc_ = {};
        initialized_ = false;
        texture_mode_ = false;
    }

} // namespace vkit
