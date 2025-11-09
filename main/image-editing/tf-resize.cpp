#include "tf-resize.h"

void resizeNearestNeighbor(const uint8_t* src, int in_width, int in_height,
                           uint8_t* dst, int out_width, int out_height) {
    for (int y = 0; y < out_height; y++) {
        int src_y = y * in_height / out_height;
        for (int x = 0; x < out_width; x++) {
            int src_x = x * in_width / out_width;
            for (int c = 0; c < 3; c++) { // R,G,B channels
                dst[(y * out_width + x) * 3 + c] =
                    src[(src_y * in_width + src_x) * 3 + c];
            }
        }
    }
}
