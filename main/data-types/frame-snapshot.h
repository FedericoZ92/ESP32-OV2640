#pragma once

#include <cstdint>
#include "esp_camera.h"

struct FrameSnapshot {
    const uint8_t* data = nullptr;
    size_t len = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    pixformat_t format = PIXFORMAT_GRAYSCALE;
    uint32_t seq = 0;
    int64_t captureUs = 0;
};