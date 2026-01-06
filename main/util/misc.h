#pragma once

#include "esp_log.h"
#include <string>


void logFirstPixels(std::string tag, 
                    std::string text,
                    const uint8_t* buffer, 
                    size_t num_pixels_to_log = 10);

