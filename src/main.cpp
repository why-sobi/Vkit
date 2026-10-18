// vkit/main.cpp
//
// Minimal pipeline: capture the primary monitor, scale it on the GPU, encode it
// to JPEG and save the bytes. The frame never leaves VRAM; only the finished
// JPEG lands in RAM, which is the buffer you would hand to send() when streaming.

#include "canvas.hpp"
#include "encoder.hpp"
#include "scaler.hpp"

#include <chrono>
#include <fstream>
#include <iostream>
#include <thread>

int main() {
    constexpr uint32_t out_width = 800;
    constexpr uint32_t out_height = 480;
    constexpr int quality = 85;

    vkit::Canvas canvas;
    if (!canvas.init() || canvas.get_monitor_offsets().empty()) {
        std::cerr << "Canvas initialization failed.\n";
        return 1;
    }

    const auto& monitor = canvas.get_monitor_offsets()[0];
    auto device = canvas.get_device();

    vkit::Scaler scaler;
    vkit::JpegEncoder encoder;
    if (!scaler.init(device.Get(), {static_cast<uint32_t>(monitor.width),
                                    static_cast<uint32_t>(monitor.height),
                                    out_width, out_height,
                                    vkit::ColorFormat::BGRA32}) ||
        !encoder.init(device.Get(), {out_width, out_height, quality})) {
        std::cerr << "GPU pipeline initialization failed.\n";
        return 1;
    }

    // capture_frame() returns false until the desktop delivers a new frame.
    bool captured = false;
    for (int attempt = 0; attempt < 100 && !captured; ++attempt) {
        captured = canvas.capture_frame();
        if (!captured) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (!captured) {
        std::cerr << "No frame captured.\n";
        return 1;
    }

    if (!scaler.scale_frame(canvas.get_context().Get(), canvas.get_frame_texture(0).Get())) {
        std::cerr << "Scaling failed.\n";
        return 1;
    }

    // View of the JPEG in RAM, valid until the next encode call.
    const auto jpeg = encoder.encode_texture(scaler.get_scaled_texture().Get());
    if (jpeg.empty()) {
        std::cerr << "JPEG encoding failed.\n";
        return 1;
    }

    std::ofstream("capture.jpg", std::ios::binary)
        .write(reinterpret_cast<const char*>(jpeg.data()), static_cast<std::streamsize>(jpeg.size()));

    std::cout << "Wrote capture.jpg (" << jpeg.size() << " bytes, "
              << (encoder.uses_video_memory() ? "GPU" : "CPU") << " input).\n";
    return 0;
}
