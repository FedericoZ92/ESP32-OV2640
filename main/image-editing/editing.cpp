#include "editing.h"
#include <cstring>
// jpeg
//#include "esp_jpeg.h" //"esp_jpeg_decoder.h"   // or <esp_jpeg.h> depending on your SDK
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "debug.h"


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

void convertRgb565ToGrayscale(const uint16_t* rgb565, uint8_t* gray, int width, int height) 
{ 
    int pixel_count = width * height; 
    for (int i = 0; i < pixel_count; ++i) { 
        uint16_t pix = rgb565[i]; 
        uint8_t r = ((pix >> 11) & 0x1F) * 255 / 31; 
        uint8_t g = ((pix >> 5) & 0x3F) * 255 / 63; 
        uint8_t b = (pix & 0x1F) * 255 / 31; 
        gray[i] = (r * 30 + g * 59 + b * 11 + 50) / 100; 
    } 
}

void convertRgb888ToGrayscale(const uint8_t* rgb, uint8_t* gray, int width, int height) 
{
    int pixel_count = width * height;
    for (int i = 0; i < pixel_count; ++i) {
        uint8_t r = rgb[i * 3];
        uint8_t g = rgb[i * 3 + 1];
        uint8_t b = rgb[i * 3 + 2];
        gray[i] = (r * 30 + g * 59 + b * 11) / 100;  // Weighted average
    }
}

bool cropCenter(const uint8_t* src, int src_width, int src_height,
                uint8_t* dst, int crop_width, int crop_height, int channels)
{
    if (!src || !dst || src_width <= 0 || src_height <= 0 ||
        crop_width <= 0 || crop_height <= 0 || channels <= 0) {
        return false;
    }

    if (crop_width > src_width || crop_height > src_height) {
        return false;
    }

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

    return true;
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

// Converts a grayscale buffer to RGB888
// grayscale: input buffer (1 byte per pixel)
// rgb: output buffer (must be pre-allocated, 3 bytes per pixel)
// width, height: dimensions of the image
void convertGrayscaleToRgb888(const uint8_t* grayscale, uint8_t* rgb, int width, int height)
{
    if (!grayscale || !rgb) return;

    int pixel_count = width * height;
    for (int i = 0; i < pixel_count; ++i) {
        uint8_t val = grayscale[i];
        rgb[i * 3 + 0] = val;  // R
        rgb[i * 3 + 1] = val;  // G
        rgb[i * 3 + 2] = val;  // B
    }
}


bool buildGray96Frame(const FrameSnapshot& snapshot,
                        uint8_t* grayscaleWorkspace,
                        size_t grayscaleWorkspaceLen,
                        uint8_t* gray96Buffer,
                        size_t tfImageInputSize)
{
    if (!snapshot.data || !grayscaleWorkspace || !gray96Buffer) {
        return false;
    }

    if (snapshot.width < tfImageInputSize || snapshot.height < tfImageInputSize) {
        ESP_LOGW(CAPTURE_TAG, "Frame too small to resize to %ux%u", (unsigned int)tfImageInputSize, (unsigned int)tfImageInputSize);
        return false;
    }

    const size_t pixelCount = (size_t)snapshot.width * (size_t)snapshot.height;
    if (pixelCount > grayscaleWorkspaceLen) {
        ESP_LOGW(CAPTURE_TAG,
                 "Frame scratch buffer too small (%u > %u)",
                 (unsigned int)pixelCount,
                 (unsigned int)grayscaleWorkspaceLen);
        return false;
    }

    switch (snapshot.format) {
        case PIXFORMAT_GRAYSCALE:
            return cropCenter(snapshot.data,
                              snapshot.width,
                              snapshot.height,
                              gray96Buffer,
                              tfImageInputSize,
                              tfImageInputSize,
                              1);

        case PIXFORMAT_RGB565:
            convertRgb565ToGrayscale((const uint16_t*)snapshot.data,
                                     grayscaleWorkspace,
                                     snapshot.width,
                                     snapshot.height);
            return cropCenter(grayscaleWorkspace,
                              snapshot.width,
                              snapshot.height,
                              gray96Buffer,
                              tfImageInputSize,
                              tfImageInputSize,
                              1);

        case PIXFORMAT_RGB888:
            convertRgb888ToGrayscale(snapshot.data,
                                     grayscaleWorkspace,
                                     snapshot.width,
                                     snapshot.height);
            return cropCenter(grayscaleWorkspace,
                              snapshot.width,
                              snapshot.height,
                              gray96Buffer,
                              tfImageInputSize,
                              tfImageInputSize,
                              1);

        case PIXFORMAT_JPEG: {
            camera_fb_t jpegFrame = {};
            jpegFrame.buf = (uint8_t*)snapshot.data;
            jpegFrame.len = snapshot.len;
            jpegFrame.width = snapshot.width;
            jpegFrame.height = snapshot.height;
            jpegFrame.format = PIXFORMAT_JPEG;

            uint8_t* decodedRgb565 = allocatingDecodeCameraJpeg(&jpegFrame,
                                                                 MALLOC_CAP_SPIRAM,
                                                                 JPEG_IMAGE_FORMAT_RGB565,
                                                                 JPEG_IMAGE_SCALE_0);
            if (!decodedRgb565) {
                ESP_LOGE(CAPTURE_TAG, "JPEG decode failed");
                return false;
            }

            convertRgb565ToGrayscale((const uint16_t*)decodedRgb565,
                                     grayscaleWorkspace,
                                     snapshot.width,
                                     snapshot.height);
            const bool ok = cropCenter(grayscaleWorkspace,
                                       snapshot.width,
                                       snapshot.height,
                                       gray96Buffer,
                                       tfImageInputSize,
                                       tfImageInputSize,
                                       1);
            heap_caps_free(decodedRgb565);
            return ok;
        }

        default:
            ESP_LOGW(CAPTURE_TAG, "Unsupported pixel format %d", snapshot.format);
            return false;
    }
}


