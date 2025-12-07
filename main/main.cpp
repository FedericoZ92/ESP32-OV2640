#include <stdio.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_camera.h>
#include "camera-driver/camera-driver.h"
#include "http-server/http-server.h"
#include "wifi/wifi.h"
#include "debug.h"
#include "gpio-config.h"
#include "tf-lite/tf-lite.h"
#include "tflite-person-detect/person_detect_model_data.h"
#include "image-editing/editing.h"
#include "led/led.h"
#include "psram/psram.h"
#include "network.h"

CameraHttpServer server;
WifiManager wifi;
LedController led;

// --- Shared static frame buffer ---
uint8_t *latest_frame = NULL;
static SemaphoreHandle_t frame_mutex = nullptr;
static TfLiteWrapper tf_wrapper;

// Background task: capture a new frame every 10 seconds
void capture_task(void *arg)
{
    ESP_LOGI(CAPTURE_TAG, "Camera capture task started");
    static uint8_t resized_frame[96 * 96];  // Grayscale input for TFLite

    if (PSRAM::isAvailable()) {
        ESP_LOGD(MAIN_TAG, "PSRAM available! Size: %d bytes\n", (int)PSRAM::getSize());
        void* ptr = PSRAM::malloc(1024);  // allocate 1 KB in PSRAM
        if (ptr) {
             ESP_LOGD(MAIN_TAG, "Allocated 1 KB in PSRAM.\n");
            PSRAM::free(ptr);
        } else {
            ESP_LOGD(MAIN_TAG, "Failed to allocate memory in PSRAM.\n");
        }
    } else {
         ESP_LOGD(MAIN_TAG, "PSRAM NOT available.\n");
         return;
    }

    latest_frame = (uint8_t*) heap_caps_malloc(160 * 120, MALLOC_CAP_SPIRAM);
    if (!latest_frame) {
        printf("Failed to allocate PSRAM buffer!\n");
        return;
    }

    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(CAPTURE_TAG, "Failed to get frame buffer");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        ESP_LOGI(CAPTURE_TAG, "Frame: %dx%d, len=%d, format=%d",
                 fb->width, fb->height, fb->len, fb->format);

        bool person_present = false;

        if (xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
            // Copy only grayscale QQVGA frames
            size_t copy_len = fb->len;
            if (copy_len > sizeof(latest_frame)) copy_len = sizeof(latest_frame);
            memcpy(latest_frame, fb->buf, copy_len);
            xSemaphoreGive(frame_mutex);

            // Resize to 96x96 for TFLite
            if (fb->width >= 96 && fb->height >= 96) {
                int channels = (fb->format == PIXFORMAT_GRAYSCALE) ? 1 : 3;
                cropCenter(fb->buf, fb->width, fb->height, resized_frame, 96, 96, channels);

                TfLiteTensor* input = tf_wrapper.getInputTensor();
                if (input && input->dims && input->dims->size >= 4) {
                    person_present = tf_wrapper.runInference(resized_frame, 96, 96);
                    ESP_LOGI(TF_TAG, "Person detected? %s", person_present ? "YES" : "NO");

                    if (person_present) {
                        led.turnLedOff();
                        led.turnBlueLedOn();
                    } else {
                        led.turnLedOff();
                        led.turnRedLedOn();
                    }
                } else {
                    ESP_LOGE(TF_TAG, "Input tensor is null or malformed");
                }
            } else {
                ESP_LOGW(CAPTURE_TAG, "Frame too small to resize to 96x96");
            }
        } else {
            ESP_LOGW(CAPTURE_TAG, "Failed to acquire frame mutex");
        }

        esp_camera_fb_return(fb);

        vTaskDelay(pdMS_TO_TICKS(10000)); // 10-second interval
    }
}

// HTTP handler — returns the latest captured frame
esp_err_t capture_http_handler(httpd_req_t *req)
{
    if (xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    esp_err_t res = httpd_resp_send(req,
                                    reinterpret_cast<const char *>(latest_frame),
                                    sizeof(latest_frame));

    xSemaphoreGive(frame_mutex);
    return res;
}

extern "C" void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_VERBOSE);

    // --- Print chip info ---
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    ESP_LOGI(OV2640_TAG, "This is %s chip with %d CPU core(s)", CONFIG_IDF_TARGET, (int)chip_info.cores);

    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(OV2640_TAG, "Get flash size failed");
        return;
    }
    ESP_LOGI(OV2640_TAG, "Flash size: %" PRIu32 " MB", flash_size / (1024 * 1024));

    // --- CAMERA RESET SEQUENCE ---
    gpio_reset_pin((gpio_num_t)PWDN_GPIO_NUM);
    gpio_reset_pin((gpio_num_t)RESET_GPIO_NUM);
    gpio_set_direction((gpio_num_t)PWDN_GPIO_NUM, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)RESET_GPIO_NUM, GPIO_MODE_OUTPUT);

    gpio_set_level((gpio_num_t)PWDN_GPIO_NUM, 0);  // Power on
    gpio_set_level((gpio_num_t)RESET_GPIO_NUM, 0); // Hold reset
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)RESET_GPIO_NUM, 1); // Release reset
    vTaskDelay(pdMS_TO_TICKS(100));

    // --- Allocate TensorFlow Lite arena intelligently ---
    uint8_t* tensor_arena = nullptr;
    const size_t arena_size = 128 * 1024; 
    if (!tensor_arena) {
        ESP_LOGW(TF_TAG, "Internal RAM low, allocating arena in PSRAM");
        tensor_arena = (uint8_t*) heap_caps_malloc(arena_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }
    if (!tensor_arena) {
        ESP_LOGE(TF_TAG, "Failed to allocate tensor arena!");
        return;
    }

    // --- Initialize TensorFlow Lite wrapper using allocated arena ---
    tf_wrapper = TfLiteWrapper(g_person_detect_model_data, arena_size);

    log_RAM_status("pre Wi-Fi initialization");

    // --- Wi-Fi initialization ---
    wifi.init();
    wifi.initSTA(WIFI_NETWORK, WIFI_PASSWORD); // connect to your network

    // --- Camera initialization ---
    CameraDriver camera;
    if (camera.init() != ESP_OK) {
        ESP_LOGE(OV2640_TAG, "Camera initialization failed");
        return;
    }

    // Flip image 180°
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_vflip(s, 1);
        s->set_hmirror(s, 1);
        ESP_LOGI(OV2640_TAG, "Camera image flipped 180°");
    } else {
        ESP_LOGW(OV2640_TAG, "Failed to get camera sensor handle for flipping");
    }

    // --- HTTP server setup ---
    frame_mutex = xSemaphoreCreateMutex();
    if (!frame_mutex) {
        ESP_LOGE(OV2640_TAG, "Failed to create frame mutex");
        return;
    }

    server.setCaptureHandler(capture_http_handler);
    if (server.start() == ESP_OK) {
        ESP_LOGI(OV2640_TAG, "HTTP Server started. Open http://%s/capture.jpg", wifi.getLocalIP().c_str());
    } else {
        ESP_LOGE(OV2640_TAG, "HTTP Server not started");
    }

    // --- Start capture task ---
    xTaskCreate(capture_task, "capture_task", 4096, nullptr, 5, nullptr);
    ESP_LOGI(OV2640_TAG, "Started periodic capture task");

    // --- Optional: run for a limited time, then reboot ---
    const uint32_t run_time_ms = 300000;  // 5 minutes
    ESP_LOGD(OV2640_TAG, "System will reboot in %d seconds...", (int)(run_time_ms / 1000));
    vTaskDelay(pdMS_TO_TICKS(run_time_ms));

    ESP_LOGW(OV2640_TAG, "Rebooting system...");
    esp_restart();
}