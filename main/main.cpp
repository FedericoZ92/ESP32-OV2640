
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "camera_driver.h"

#include "config.h"

#define OV2640_TAG "ov2640"

extern "C" void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_VERBOSE); 
    // Print chip info
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    ESP_LOGI(OV2640_TAG, "This is %s chip with %d CPU core(s)", CONFIG_IDF_TARGET, chip_info.cores);

    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(OV2640_TAG, "Get flash size failed");
        return;
    }
    ESP_LOGI(OV2640_TAG, "Flash size: %" PRIu32 " MB", flash_size / (1024*1024));

    // --- CAMERA RESET SEQUENCE ---
    gpio_reset_pin((gpio_num_t)PWDN_GPIO_NUM);
    gpio_reset_pin((gpio_num_t)RESET_GPIO_NUM);
    gpio_set_direction((gpio_num_t)PWDN_GPIO_NUM, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)RESET_GPIO_NUM, GPIO_MODE_OUTPUT);

    gpio_set_level((gpio_num_t)PWDN_GPIO_NUM, 0);  // Power on
    gpio_set_level((gpio_num_t)RESET_GPIO_NUM, 0); // Hold reset
    vTaskDelay(pdMS_TO_TICKS(10));     // Wait 10 ms
    gpio_set_level((gpio_num_t)RESET_GPIO_NUM, 1); // Release reset
   vTaskDelay(pdMS_TO_TICKS(100)); // Wait longer for camera startup

    // Camera
    CameraDriver camera;
    if (camera.init() != ESP_OK) {
        ESP_LOGE(OV2640_TAG, "Camera initialization failed");
        return;
    }

    camera_fb_t* fb = camera.captureFrame();
    if (fb) {
        char line[128];
        size_t line_len = 0;
        for (size_t i = 0; i < fb->len; i++) {
            line_len += snprintf(line + line_len, sizeof(line) - line_len, "%02X ", fb->buf[i]);
            if ((i + 1) % 16 == 0 || i == fb->len - 1) {
                ESP_LOGI(OV2640_TAG, "%s", line);
                line_len = 0;
            }
        }
        ESP_LOGI(OV2640_TAG, "Total bytes: %zu", fb->len);
        camera.releaseFrame(fb);
    }

    // Restart countdown
    for (int i = 10; i >= 0; i--) {
        ESP_LOGI(OV2640_TAG, "Restarting in %d seconds...", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    ESP_LOGI(OV2640_TAG, "Restarting now.");
    esp_restart();
}