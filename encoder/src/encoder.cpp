#include "encoder.hpp"

#include <iostream>

#ifdef VKIT_HAS_NVJPEG
    #include <cuda_runtime_api.h>
    #include <cuda_d3d11_interop.h>
    #include <nvjpeg.h>

// Implemented in encoder_cuda.cu. A .cpp file is compiled as ordinary C++ and
// cannot contain CUDA kernel launches, so the small GPU conversion helper lives
// in a CUDA source file and is exposed through this C-linkage function.
extern "C" bool vkit_convert_bgra_array_to_rgb(
    cudaArray_t source,
    unsigned char* destination,
    uint32_t width,
    uint32_t height,
    cudaStream_t stream);
#endif

namespace vkit {

namespace {

#ifdef VKIT_HAS_NVJPEG
bool check_cuda(cudaError_t status, const char* operation) {
    if (status == cudaSuccess) return true;
    std::cerr << "[JpegEncoder] " << operation << " failed: "
              << cudaGetErrorString(status) << '\n';
    return false;
}

bool check_nvjpeg(nvjpegStatus_t status, const char* operation) {
    if (status == NVJPEG_STATUS_SUCCESS) return true;
    std::cerr << "[JpegEncoder] " << operation
              << " failed with nvJPEG status " << static_cast<int>(status) << '\n';
    return false;
}
#endif

} // namespace

JpegEncoder::~JpegEncoder() {
    cleanup();
}

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

#ifndef VKIT_HAS_NVJPEG
    std::cerr << "[JpegEncoder] nvJPEG is unavailable. Install the CUDA Toolkit.\n";
    return false;
#else
    nvjpegHandle_t handle = nullptr;
    nvjpegEncoderState_t encode_state = nullptr;
    nvjpegEncoderParams_t encode_params = nullptr;
    cudaStream_t stream = nullptr;

    if (d3d_device) {
        // D3D11 and CUDA must use the same physical NVIDIA adapter. Selecting
        // CUDA device 0 blindly would be wrong on multi-GPU systems.
        unsigned int device_count = 0;
        int devices[8]{};
        if (!check_cuda(cudaD3D11GetDevices(
                            &device_count, devices, 8, d3d_device,
                            cudaD3D11DeviceListAll),
                        "cudaD3D11GetDevices") ||
            device_count == 0 || devices[0] < 0 ||
            !check_cuda(cudaSetDevice(devices[0]), "cudaSetDevice")) {
            return false;
        }
    }

    if (!check_nvjpeg(nvjpegCreateSimple(&handle), "nvjpegCreateSimple") ||
        !check_nvjpeg(nvjpegEncoderStateCreate(handle, &encode_state, nullptr),
                      "nvjpegEncoderStateCreate") ||
        !check_nvjpeg(nvjpegEncoderParamsCreate(handle, &encode_params, nullptr),
                      "nvjpegEncoderParamsCreate") ||
        !check_nvjpeg(nvjpegEncoderParamsSetQuality(
                  encode_params, desc.quality, nullptr),
                      "nvjpegEncoderParamsSetQuality") ||
        !check_nvjpeg(nvjpegEncoderParamsSetSamplingFactors(
                          encode_params, NVJPEG_CSS_444, nullptr),
                      "nvjpegEncoderParamsSetSamplingFactors") ||
        !check_cuda(cudaStreamCreateWithFlags(&stream, cudaStreamNonBlocking),
                    "cudaStreamCreateWithFlags")) {
        if (stream) cudaStreamDestroy(stream);
        if (encode_params) nvjpegEncoderParamsDestroy(encode_params);
        if (encode_state) nvjpegEncoderStateDestroy(encode_state);
        if (handle) nvjpegDestroy(handle);
        return false;
    }

    void* device_rgb = nullptr;
    // nvJPEG receives RGBI (interleaved RGB), so reserve 3 bytes per pixel on
    // the GPU. This buffer is reused for every frame.
    const size_t rgb_size = static_cast<size_t>(desc.width) *
                            static_cast<size_t>(desc.height) * 3;
    if (!check_cuda(cudaMalloc(&device_rgb, rgb_size), "cudaMalloc")) {
        cudaStreamDestroy(stream);
        nvjpegEncoderParamsDestroy(encode_params);
        nvjpegEncoderStateDestroy(encode_state);
        nvjpegDestroy(handle);
        return false;
    }

    desc_ = desc;
    handle_ = handle;
    encode_state_ = encode_state;
    encode_params_ = encode_params;
    stream_ = stream;
    device_rgb_ = device_rgb;
    texture_mode_ = d3d_device != nullptr;
    return true;
#endif
}

bool JpegEncoder::encode_bgra(
    const uint8_t* pixels,
    size_t row_pitch,
    std::vector<uint8_t>& jpeg_output) {
    jpeg_output.clear();

    if (!pixels || desc_.width == 0 || desc_.height == 0 ||
        row_pitch < static_cast<size_t>(desc_.width) * 4) {
        std::cerr << "[JpegEncoder] Invalid BGRA input.\n";
        return false;
    }

#ifndef VKIT_HAS_NVJPEG
    std::cerr << "[JpegEncoder] Cannot encode because nvJPEG is unavailable.\n";
    return false;
#else
    auto handle = static_cast<nvjpegHandle_t>(handle_);
    auto encode_state = static_cast<nvjpegEncoderState_t>(encode_state_);
    auto encode_params = static_cast<nvjpegEncoderParams_t>(encode_params_);
    auto stream = static_cast<cudaStream_t>(stream_);
    auto device_rgb = static_cast<unsigned char*>(device_rgb_);

    if (!handle || !encode_state || !encode_params || !stream || !device_rgb) {
        std::cerr << "[JpegEncoder] Encoder is not initialized.\n";
        return false;
    }

    const size_t rgb_pitch = static_cast<size_t>(desc_.width) * 3;
    std::vector<uint8_t> rgb(rgb_pitch * desc_.height);
    for (uint32_t y = 0; y < desc_.height; ++y) {
        const uint8_t* src = pixels + static_cast<size_t>(y) * row_pitch;
        uint8_t* dst = rgb.data() + static_cast<size_t>(y) * rgb_pitch;
        for (uint32_t x = 0; x < desc_.width; ++x) {
            dst[x * 3 + 0] = src[x * 4 + 2];
            dst[x * 3 + 1] = src[x * 4 + 1];
            dst[x * 3 + 2] = src[x * 4 + 0];
        }
    }

    if (!check_cuda(cudaMemcpyAsync(
                        device_rgb, rgb.data(), rgb.size(),
                        cudaMemcpyHostToDevice, stream),
                    "cudaMemcpyAsync")) {
        return false;
    }

    nvjpegImage_t image{};
    image.channel[0] = device_rgb;
    image.pitch[0] = static_cast<unsigned int>(rgb_pitch);

    if (!check_nvjpeg(nvjpegEncodeImage(
                          handle, encode_state, encode_params, &image,
                          NVJPEG_INPUT_RGBI, desc_.width, desc_.height, stream),
                      "nvjpegEncodeImage")) {
        return false;
    }

    if (!check_cuda(cudaStreamSynchronize(stream), "cudaStreamSynchronize")) {
        return false;
    }

    size_t output_size = 0;
    if (!check_nvjpeg(nvjpegEncodeRetrieveBitstream(
                          handle, encode_state, nullptr, &output_size, stream),
                      "nvjpegEncodeRetrieveBitstream(size)")) {
        return false;
    }

    jpeg_output.resize(output_size);
    if (!check_nvjpeg(nvjpegEncodeRetrieveBitstream(
                          handle, encode_state, jpeg_output.data(), &output_size, stream),
                      "nvjpegEncodeRetrieveBitstream(data)")) {
        jpeg_output.clear();
        return false;
    }

    jpeg_output.resize(output_size);
    return true;
#endif
}

bool JpegEncoder::encode_texture(
    ID3D11Texture2D* texture,
    std::vector<uint8_t>& jpeg_output) {
    jpeg_output.clear();

#ifndef VKIT_HAS_NVJPEG
    (void)texture;
    std::cerr << "[JpegEncoder] Cannot encode because nvJPEG is unavailable.\n";
    return false;
#else
    if (!texture_mode_ || !texture || !stream_ || !device_rgb_) {
        std::cerr << "[JpegEncoder] Texture encoding is not initialized.\n";
        return false;
    }

    auto stream = static_cast<cudaStream_t>(stream_);
    auto resource = static_cast<cudaGraphicsResource_t>(graphics_resource_);
    if (!resource) {
        // Registration connects this D3D11 resource to CUDA. It does not copy
        // the image; it lets CUDA access the texture in place.
        if (!check_cuda(cudaGraphicsD3D11RegisterResource(
                            &resource, texture, cudaGraphicsRegisterFlagsNone),
                        "cudaGraphicsD3D11RegisterResource")) {
            return false;
        }
        graphics_resource_ = resource;
    }

    if (!check_cuda(cudaGraphicsMapResources(1, &resource, stream),
                    "cudaGraphicsMapResources")) {
        return false;
    }

    // A mapped D3D11 texture is exposed as a CUDA array. The CUDA kernel reads
    // that array and writes a linear RGB buffer that nvJPEG can consume.
    cudaArray_t source = nullptr;
    bool success = check_cuda(cudaGraphicsSubResourceGetMappedArray(
                                  &source, resource, 0, 0),
                              "cudaGraphicsSubResourceGetMappedArray");
    if (success) {
        success = vkit_convert_bgra_array_to_rgb(
            source, static_cast<unsigned char*>(device_rgb_),
            desc_.width, desc_.height, stream);
    }
    if (success) {
        success = check_cuda(cudaStreamSynchronize(stream),
                             "cudaStreamSynchronize(color conversion)");
    }

    // The graphics resource must be unmapped before D3D11 uses it again.
    const cudaError_t unmap_status = cudaGraphicsUnmapResources(1, &resource, stream);
    if (!success || !check_cuda(unmap_status, "cudaGraphicsUnmapResources")) {
        return false;
    }

    auto handle = static_cast<nvjpegHandle_t>(handle_);
    auto encode_state = static_cast<nvjpegEncoderState_t>(encode_state_);
    auto encode_params = static_cast<nvjpegEncoderParams_t>(encode_params_);
    nvjpegImage_t image{};
    image.channel[0] = static_cast<unsigned char*>(device_rgb_);
    image.pitch[0] = static_cast<size_t>(desc_.width) * 3;

    if (!check_nvjpeg(nvjpegEncodeImage(
                          handle, encode_state, encode_params, &image,
                          NVJPEG_INPUT_RGBI, desc_.width, desc_.height, stream),
                      "nvjpegEncodeImage")) {
        return false;
    }
    if (!check_cuda(cudaStreamSynchronize(stream), "cudaStreamSynchronize(encode)")) {
        return false;
    }

    // JPEG compression happened on the GPU. Only the compressed bitstream is
    // copied into jpeg_output; the full image is never read back to the CPU.
    size_t output_size = 0;
    if (!check_nvjpeg(nvjpegEncodeRetrieveBitstream(
                          handle, encode_state, nullptr, &output_size, stream),
                      "nvjpegEncodeRetrieveBitstream(size)")) {
        return false;
    }
    jpeg_output.resize(output_size);
    if (!check_nvjpeg(nvjpegEncodeRetrieveBitstream(
                          handle, encode_state, jpeg_output.data(), &output_size, stream),
                      "nvjpegEncodeRetrieveBitstream(data)")) {
        jpeg_output.clear();
        return false;
    }
    jpeg_output.resize(output_size);
    return true;
#endif
}

void JpegEncoder::cleanup() {
#ifdef VKIT_HAS_NVJPEG
    if (graphics_resource_) {
        // Unregister before releasing the encoder's other CUDA resources.
        cudaGraphicsUnregisterResource(
            static_cast<cudaGraphicsResource_t>(graphics_resource_));
    }
    if (device_rgb_) cudaFree(device_rgb_);
    if (stream_) cudaStreamDestroy(static_cast<cudaStream_t>(stream_));
    if (encode_params_) {
        nvjpegEncoderParamsDestroy(
            static_cast<nvjpegEncoderParams_t>(encode_params_));
    }
    if (encode_state_) {
        nvjpegEncoderStateDestroy(
            static_cast<nvjpegEncoderState_t>(encode_state_));
    }
    if (handle_) nvjpegDestroy(static_cast<nvjpegHandle_t>(handle_));
#endif

    stream_ = nullptr;
    encode_params_ = nullptr;
    encode_state_ = nullptr;
    handle_ = nullptr;
    device_rgb_ = nullptr;
    graphics_resource_ = nullptr;
    texture_mode_ = false;
    desc_ = {};
}

} // namespace vkit
