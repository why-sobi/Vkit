#define CANVAS_VRAM_RESIDENCE
#include "canvas.hpp"

int main() {
    vkit::Canvas canvas;
    canvas.init();

    while (canvas.capture_frame());

    return 0;
}