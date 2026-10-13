// vkit/main.cpp

#include "canvas.hpp"
#include "encoder.hpp"
#include "scaler.hpp"

#include <fstream>
#include <iostream>
#include <vector>

int main() {
    vkit::Canvas canvas;
    vkit::Scaler scaler;
    vkit::JpegEncoder encoder;

    if (!canvas.init()) {
        std::cerr << "[Test] Canvas initialization failed.\n";
        return 1;
    }

    // Scale and encode monitor 0 using GPU textures throughout.
    const auto& monitor_offsets = canvas.get_monitor_offsets();
    if (monitor_offsets.empty()) {
        std::cerr << "[Test] No monitors available.\n";
        return 1;
    }

    const uint32_t width = static_cast<uint32_t>(monitor_offsets[0].width);
    const uint32_t height = static_cast<uint32_t>(monitor_offsets[0].height);
    constexpr uint32_t output_width = 800;
    constexpr uint32_t output_height = 480;
    vkit::ScalerDesc scale_desc{
        width, height, output_width, output_height,
        vkit::ColorFormat::BGRA32
    };

    if (!scaler.init(canvas.get_device().Get(), scale_desc) ||
        !encoder.init(canvas.get_device().Get(),
                      {output_width, output_height, 85})) {
        std::cerr << "[Test] GPU pipeline initialization failed.\n";
        return 1;
    }

    if (canvas.capture_frame()) {
        if (!scaler.scale_frame(
                canvas.get_context().Get(),
                canvas.get_frame_texture(0).Get())) {
            std::cerr << "[Test] Scaling failed.\n";
            return 1;
        }

        std::vector<uint8_t> jpeg;
        if (!encoder.encode_texture(scaler.get_scaled_texture().Get(), jpeg)) {
            std::cerr << "[Test] GPU JPEG encoding failed.\n";
            return 1;
        }

        std::ofstream output("capture.jpg", std::ios::binary);
        if (!output) {
            std::cerr << "[Test] Could not open capture.jpg for writing.\n";
            return 1;
        }
        output.write(reinterpret_cast<const char*>(jpeg.data()),
                     static_cast<std::streamsize>(jpeg.size()));
        if (!output) {
            std::cerr << "[Test] Could not write capture.jpg.\n";
            return 1;
        }
        std::cout << "[Test] GPU JPEG encoding succeeded: "
                  << jpeg.size() << " bytes.\n";
    }

    return 0;
}