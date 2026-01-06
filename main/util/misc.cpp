#include "misc.h"

// Logs the first N pixels of a buffer
void logFirstPixels(std::string tag, 
                    std::string text,
                    const uint8_t* buffer,  
                    size_t num_pixels_to_log)
{
    std::string pixels_str = "";
    for (size_t i = 0; i < num_pixels_to_log; ++i) {
        pixels_str += std::to_string(buffer[i]);
        if (i < num_pixels_to_log - 1) pixels_str += ", ";
    }

    std::string msg = text + ", first " + std::to_string(num_pixels_to_log) + " pixels: " + pixels_str;
    ESP_LOGD(tag.c_str(), "%s", msg.c_str());
}

