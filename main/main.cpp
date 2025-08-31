
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "camera_driver.h"

#define TAG "APP_MAIN"

extern "C" void app_main(void)
{
    // Print chip info
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU core(s)\n", CONFIG_IDF_TARGET, chip_info.cores);
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed\n");
        return;
    }
    printf("Flash size: %" PRIu32 " MB\n", flash_size / (1024*1024));

    // Camera
    CameraDriver camera;
    if (camera.init() != ESP_OK) {
        ESP_LOGE(TAG, "Camera initialization failed");
        return;
    }

    camera_fb_t* fb = camera.captureFrame();
    if (fb) {
        // Print first few bytes
        for (int i = 0; i < 16 && i < fb->len; i++) {
            printf("%02X ", fb->buf[i]);
        }
        printf("\n");

        camera.releaseFrame(fb);
    }

    // Restart countdown
    for (int i = 10; i >= 0; i--) {
        printf("Restarting in %d seconds...\n", i);
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
    printf("Restarting now.\n");
    fflush(stdout);
    esp_restart();
}