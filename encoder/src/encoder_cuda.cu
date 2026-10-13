#include <cstdint>
#include <cuda_runtime.h>

// One CUDA thread handles one pixel. The source is a BGRA8 D3D11 texture
// exposed as a CUDA surface; the destination is tightly packed RGB data for
// nvJPEG's NVJPEG_INPUT_RGBI format.
extern "C" __global__ void vkit_bgra_to_rgb_kernel(
    cudaSurfaceObject_t source,
    unsigned char* destination,
    unsigned int width,
    unsigned int height) {
    const unsigned int x = blockIdx.x * blockDim.x + threadIdx.x;
    const unsigned int y = blockIdx.y * blockDim.y + threadIdx.y;
    if (x >= width || y >= height) return;

    const uchar4 pixel = surf2Dread<uchar4>(
        source, static_cast<int>(x * sizeof(uchar4)), y);
    unsigned char* output = destination +
        (static_cast<size_t>(y) * width + x) * 3;
    output[0] = pixel.z; // R
    output[1] = pixel.y; // G
    output[2] = pixel.x; // B
}

// Bridges the CUDA array returned by D3D11 interop to the conversion kernel.
// This function is called from encoder.cpp, while the kernel above is compiled
// by nvcc from this .cu file.
extern "C" bool vkit_convert_bgra_array_to_rgb(
    cudaArray_t source,
    unsigned char* destination,
    uint32_t width,
    uint32_t height,
    cudaStream_t stream) {
    cudaResourceDesc resource_desc{};
    resource_desc.resType = cudaResourceTypeArray;
    resource_desc.res.array.array = source;

    cudaSurfaceObject_t surface = 0;
    cudaError_t status = cudaCreateSurfaceObject(&surface, &resource_desc);
    if (status != cudaSuccess) return false;

    const dim3 block(16, 16);
    const dim3 grid(
        (width + block.x - 1) / block.x,
        (height + block.y - 1) / block.y);
    vkit_bgra_to_rgb_kernel<<<grid, block, 0, stream>>>(
        surface, destination, width, height);

    status = cudaGetLastError();
    cudaDestroySurfaceObject(surface);
    return status == cudaSuccess;
}
