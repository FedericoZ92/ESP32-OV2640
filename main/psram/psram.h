
#pragma once

#include <cstddef>    // for size_t
#include <cstdlib>    // for NULL
#include "esp_heap_caps.h"
#include "esp_log.h"

class PSRAM {
public:
    // Check if PSRAM is available
    static bool isAvailable() {
        size_t psram_size = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        ESP_LOGI("PSRAM", "Total PSRAM size: %d bytes", psram_size);

        if (psram_size > 0) {
            ESP_LOGI("PSRAM", "PSRAM is available and enabled");
        } else {
            ESP_LOGI("PSRAM", "PSRAM not detected");
            
        }
        return psram_size > 0;
    }

    // Get total PSRAM size
    static size_t getSize() {
        if (isAvailable()) {
            return heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
        }
        return 0;
    }

    // Allocate memory in PSRAM
    static void* malloc(size_t size) {
        if (!isAvailable() || size == 0) return nullptr;
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM);
    }

    // Free memory allocated in PSRAM
    static void free(void* ptr) {
        if (ptr) heap_caps_free(ptr);
    }
};

