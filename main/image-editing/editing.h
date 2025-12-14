#pragma once
#include <stdint.h>
#include "esp_camera.h"

// Resize an RGB888 image using nearest neighbor
// src: pointer to input image (width x height x 3)
// dst: pointer to output image (out_width x out_height x 3)
// in_width, in_height: input image dimensions
// out_width, out_height: output image dimensions
void resizeRgbNearestNeighbor(const uint8_t* src, 
                            int in_width, int in_height,
                            uint8_t* dst, 
                            int out_width, int out_height);

void convertRgb888ToGrayscale(const uint8_t* rgb, 
                            uint8_t* gray, 
                            int width, 
                            int height);

void cropCenter(uint8_t* src, 
                int src_width, 
                int src_height, 
                uint8_t* dst, 
                int crop_width, 
                int crop_height, 
                int channels);

uint8_t* allocatingDecodeCameraJpeg(camera_fb_t *fb, 
                                    uint32_t memory_type,
                                    esp_jpeg_image_format_t out_format,
                                    esp_jpeg_image_scale_t scale);