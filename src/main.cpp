// vkit/main.cpp

#include "canvas.hpp"
#include "scaler.hpp"

#include <iostream>

int main() {
    vkit::Canvas canvas;
    vkit::Scaler scaler;

    canvas.init();

    // Request a downscale test: Native resolution -> 800x480
    auto master_res = canvas.get_master_resolution();
    vkit::ScalerDesc desc{
        static_cast<uint32_t>(master_res.first),
        static_cast<uint32_t>(master_res.second),
        800, 
        480, 
        vkit::ColorFormat::BGRA32 // native texture format for D3D11
    };

    scaler.init(canvas.get_device().Get(), desc);

    if (canvas.capture_frame()) {
        // Pass the VRAM texture from canvas to scaler
        scaler.scale_frame(canvas.get_context().Get(), canvas.get_frame_texture(0).Get());
        std::cout << "[Test] Scaling executed successfully!\n";
    }
}   