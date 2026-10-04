#include "canvas.hpp"

int main() {
    vkit::Canvas canvas;
    canvas.init();

    while (canvas.capture_frame());

    canvas.saveMasterPpm("../../output.ppm"); // Save the captured frame to a PPM file for testing
    
    return 0;
}