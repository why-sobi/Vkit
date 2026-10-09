#define CANVAS_VRAM_RESIDENCE
#define CANVAS_MOUSE_DISPLAY
// #define CANVAS_DIRTY_RECT_OPT
#include "canvas.hpp"

int main() {
    vkit::Canvas canvas;
    canvas.init();

    while (canvas.capture_frame());

    // canvas.saveMasterPpm("../../master_canvas.ppm");

    return 0;
}