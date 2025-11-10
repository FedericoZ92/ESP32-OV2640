#include "editing.h"

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

void convertRGB888ToGrayscale(const uint8_t* rgb, uint8_t* gray, int width, int height) {
    int pixel_count = width * height;
    for (int i = 0; i < pixel_count; ++i) {
        uint8_t r = rgb[i * 3];
        uint8_t g = rgb[i * 3 + 1];
        uint8_t b = rgb[i * 3 + 2];
        gray[i] = (r * 30 + g * 59 + b * 11) / 100;  // Weighted average
    }
}
