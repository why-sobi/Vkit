// encoder/tests/jpeg_encoder_test.cpp
//
// Smoke test for whichever backend vkit::JpegEncoder resolves to. Encodes a
// test pattern from CPU memory and from a D3D11 texture, and checks that each
// result is a complete JPEG in RAM. Writes test_cpu.jpg / test_gpu.jpg.

#include "encoder.hpp"

#include <d3d11.h>
#include <wrl/client.h>

#include <cstdint>
#include <fstream>
#include <iostream>
#include <span>
#include <vector>

using Microsoft::WRL::ComPtr;

namespace {

    int failures = 0;

    void check(bool ok, const char* what) {
        std::cout << (ok ? "[PASS] " : "[FAIL] ") << what << '\n';
        if (!ok) ++failures;
    }

    // A JPEG starts with SOI (FF D8) and ends with EOI (FF D9).
    bool is_jpeg(std::span<const uint8_t> data) {
        return data.size() > 4 &&
               data[0] == 0xFF && data[1] == 0xD8 &&
               data[data.size() - 2] == 0xFF && data[data.size() - 1] == 0xD9;
    }

    void save(const char* filename, std::span<const uint8_t> data) {
        std::ofstream(filename, std::ios::binary)
            .write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
    }

    // Red, green, blue and white quadrants in BGRA.
    std::vector<uint8_t> make_quadrants(uint32_t width, uint32_t height) {
        std::vector<uint8_t> bgra(static_cast<size_t>(width) * height * 4);
        for (uint32_t y = 0; y < height; ++y) {
            for (uint32_t x = 0; x < width; ++x) {
                const bool left = x < width / 2, top = y < height / 2;
                uint8_t r = 255, g = 255, b = 255;
                if (left && top)       { g = 0; b = 0; }
                else if (!left && top) { r = 0; b = 0; }
                else if (left)         { r = 0; g = 0; }
                uint8_t* p = bgra.data() + (static_cast<size_t>(y) * width + x) * 4;
                p[0] = b; p[1] = g; p[2] = r; p[3] = 255;
            }
        }
        return bgra;
    }

} // namespace

int main() {
    constexpr uint32_t width = 640;
    constexpr uint32_t height = 360;
    constexpr size_t row_pitch = width * 4;
    const auto pixels = make_quadrants(width, height);

    // CPU input
    {
        vkit::JpegEncoder encoder;
        check(encoder.init({width, height, 85}), "CPU: init");

        const auto jpeg = encoder.encode_bgra(pixels.data(), row_pitch);
        check(is_jpeg(jpeg), "CPU: encode_bgra returns a complete JPEG");
        save("test_cpu.jpg", jpeg);

        check(encoder.encode_bgra(nullptr, row_pitch).empty(), "CPU: null input returns an empty view");
    }

    // GPU input
    {
        ComPtr<ID3D11Device> device;
        D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT,
                          nullptr, 0, D3D11_SDK_VERSION, device.GetAddressOf(), nullptr, nullptr);

        D3D11_TEXTURE2D_DESC desc{};
        desc.Width = width;
        desc.Height = height;
        desc.MipLevels = 1;
        desc.ArraySize = 1;
        desc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        desc.SampleDesc.Count = 1;
        desc.Usage = D3D11_USAGE_DEFAULT;
        desc.BindFlags = D3D11_BIND_RENDER_TARGET | D3D11_BIND_SHADER_RESOURCE;
        const D3D11_SUBRESOURCE_DATA init_data{pixels.data(), static_cast<UINT>(row_pitch), 0};

        ComPtr<ID3D11Texture2D> texture;
        check(device && SUCCEEDED(device->CreateTexture2D(&desc, &init_data, texture.GetAddressOf())),
              "GPU: create D3D11 device and texture");

        vkit::JpegEncoder encoder;
        check(encoder.init(device.Get(), {width, height, 85}), "GPU: init");

        const auto jpeg = encoder.encode_texture(texture.Get());
        check(is_jpeg(jpeg), "GPU: encode_texture returns a complete JPEG");
        save("test_gpu.jpg", jpeg);

        // The view points at the encoder's reused output buffer.
        const auto again = encoder.encode_texture(texture.Get());
        check(is_jpeg(again) && again.data() == jpeg.data(), "GPU: output buffer is reused between frames");

        check(encoder.encode_texture(nullptr).empty(), "GPU: null texture returns an empty view");
    }

    std::cout << (failures ? "FAILED" : "All checks passed") << '\n';
    return failures ? 1 : 0;
}
