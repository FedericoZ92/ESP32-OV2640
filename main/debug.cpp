#include "debug.h"
extern "C" {
    #include "esp_system.h"
    #include "esp_heap_caps.h"
}

void log_RAM_status() {
    esp_log_level_set(RAM_TAG, ESP_LOG_VERBOSE);

    size_t free_heap = esp_get_free_heap_size();
    size_t largest_block = heap_caps_get_largest_free_block(MALLOC_CAP_8BIT);

    ESP_LOGD(RAM_TAG, "Free heap: %d bytes", (int)free_heap);
    ESP_LOGD(RAM_TAG, "Largest allocatable block (8-bit): %d bytes", (int)largest_block);
}