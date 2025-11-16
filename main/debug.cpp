#include "debug.h"
extern "C" {
    #include "esp_system.h"
    #include "esp_heap_caps.h"
}

void log_RAM_status(const std::string& header) {
    ///esp_log_level_set(RAM_TAG, ESP_LOG_VERBOSE);

    ESP_LOGI(RAM_TAG, "------ RAM STATUS: %s, ------", header.c_str());
    ESP_LOGI(RAM_TAG, "Free heap: %d bytes", (int)esp_get_free_heap_size());
    ESP_LOGI(RAM_TAG, "Largest block: %d bytes", (int)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    ESP_LOGI(RAM_TAG, "Internal free: %d bytes", (int)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    ESP_LOGI(RAM_TAG, "DMA free: %d bytes", (int)heap_caps_get_free_size(MALLOC_CAP_DMA));
    ESP_LOGI(RAM_TAG, "---------------------------------------------");
}