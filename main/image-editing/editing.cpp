#include "editing.h"
#include <cstring>
// jpeg
//#include "esp_jpeg.h" //"esp_jpeg_decoder.h"   // or <esp_jpeg.h> depending on your SDK
#include "esp_heap_caps.h"
#include "esp_log.h"

void resizeRgbNearestNeighbor(const uint8_t* src, int in_width, int in_height,
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

void convertRgb888ToGrayscale(const uint8_t* rgb, uint8_t* gray, int width, int height) {
    int pixel_count = width * height;
    for (int i = 0; i < pixel_count; ++i) {
        uint8_t r = rgb[i * 3];
        uint8_t g = rgb[i * 3 + 1];
        uint8_t b = rgb[i * 3 + 2];
        gray[i] = (r * 30 + g * 59 + b * 11) / 100;  // Weighted average
    }
}

void cropCenter(uint8_t* src, int src_width, int src_height, 
                uint8_t* dst, int crop_width, int crop_height, int channels)
{
    // Calculate top-left corner of the crop region
    int start_x = (src_width  - crop_width)  / 2;
    int start_y = (src_height - crop_height) / 2;

    // Safety clamp
    if (start_x < 0) start_x = 0;
    if (start_y < 0) start_y = 0;

    for (int y = 0; y < crop_height; ++y) {
        const uint8_t* src_row = src + ((start_y + y) * src_width + start_x) * channels;
        uint8_t* dst_row = dst + (y * crop_width) * channels;
        memcpy(dst_row, src_row, crop_width * channels);
    }
}

uint8_t* allocatingDecodeCameraJpeg(camera_fb_t *fb, 
                                            uint32_t memory_type,
                                            esp_jpeg_image_format_t out_format,
                                            esp_jpeg_image_scale_t scale)
{
    if (!fb || fb->format != PIXFORMAT_JPEG) return NULL;

    int width  = fb->width;
    int height = fb->height;
    size_t out_size = width * height * 2;  // RGB565

    uint8_t* out_buf = (uint8_t*) heap_caps_malloc(out_size, memory_type );
    if (!out_buf) {
        ESP_LOGE("JPEG_CONVERT", "Malloc failed");
        return NULL;
    }

    esp_jpeg_image_cfg_t cfg = {};
    cfg.indata       = fb->buf;
    cfg.indata_size  = fb->len;
    cfg.outbuf       = out_buf;
    cfg.outbuf_size  = out_size;
    cfg.out_format   = out_format;
    cfg.out_scale    = scale;

    esp_jpeg_image_output_t out = {};

    if (esp_jpeg_decode(&cfg, &out) != ESP_OK) {
        heap_caps_free(out_buf);
        return NULL;
    }

    return out_buf;   // caller frees
}
