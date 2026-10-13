#include "encoder.hpp"

#include <cstdint>
#include <fstream>
#include <iostream>
#include <vector>

#ifdef _WIN32
    #include <windows.h>
    #include <shellapi.h>
#endif

int main() {
    constexpr uint32_t width = 256;
    constexpr uint32_t height = 256;
    constexpr size_t bytes_per_pixel = 4;
    constexpr size_t row_pitch = width * bytes_per_pixel;

    std::vector<uint8_t> bgra(row_pitch * height);
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const bool left = x < width / 2;
            const bool top = y < height / 2;

            uint8_t red = 255;
            uint8_t green = 255;
            uint8_t blue = 255;
            if (left && top) {
                red = 255; green = 0; blue = 0;
            } else if (!left && top) {
                red = 0; green = 255; blue = 0;
            } else if (left && !top) {
                red = 0; green = 0; blue = 255;
            }

            uint8_t* pixel = bgra.data() + static_cast<size_t>(y) * row_pitch +
                             static_cast<size_t>(x) * bytes_per_pixel;
            pixel[0] = blue;
            pixel[1] = green;
            pixel[2] = red;
            pixel[3] = 255;
        }
    }

    vkit::JpegEncoder encoder;
    if (!encoder.init({width, height, 85})) return 1;

    std::vector<uint8_t> jpeg;
    if (!encoder.encode_bgra(bgra.data(), row_pitch, jpeg)) return 1;

    const char* filename = "test.jpg";
    std::ofstream output(filename, std::ios::binary);
    if (!output) {
        std::cerr << "Could not open " << filename << " for writing.\n";
        return 1;
    }
    output.write(reinterpret_cast<const char*>(jpeg.data()),
                 static_cast<std::streamsize>(jpeg.size()));
    if (!output) {
        std::cerr << "Could not write " << filename << ".\n";
        return 1;
    }

    std::cout << "Wrote " << filename << " (" << jpeg.size() << " bytes).\n";
    std::cout << "Verify red, green, blue, and white quadrants in the image.\n";

#ifdef _WIN32
    ShellExecuteA(nullptr, "open", filename, nullptr, nullptr, SW_SHOWNORMAL);
#endif
    return 0;
}
