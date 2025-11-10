#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_system.h"
#include "esp_log.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_camera.h"
#include "camera_driver.h"
#include "http-server.h"
#include "wifi.h"
#include "debug.h"
#include "config.h"
#include "tf-lite.h"
#include "tflite-person-detect/person_detect_model_data.h"
#include "image-editing/tf-resize.h"
#include <vector>

CameraHttpServer server;
WifiManager wifi;

// Shared frame buffer
static std::vector<uint8_t> latest_frame;
static SemaphoreHandle_t frame_mutex = nullptr;
static TfLiteWrapper tf_wrapper;

// Background task: capture a new frame every 10 seconds
void capture_task(void *arg)
{
    ESP_LOGI(CAPTURE_TAG, "Camera capture task started");
    static uint8_t rgb_frame[160 * 120 * 3];     // Adjust based on camera resolution
    static uint8_t resized_frame[96 * 96 * 3];   // 96x96 RGB buffer for model
    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            if (xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
                // Copy raw frame to shared buffer (optional, e.g., for HTTP)
                latest_frame.assign(fb->buf, fb->buf + fb->len);
                xSemaphoreGive(frame_mutex);

                // Decode JPEG to RGB888 if needed
                if (fb->format == PIXFORMAT_JPEG) {
                    bool success = fmt2rgb888(fb->buf, fb->len, PIXFORMAT_RGB888, rgb_frame);
                    if (!success) {
                        ESP_LOGE(CAPTURE_TAG, "JPEG decode failed");
                        esp_camera_fb_return(fb);
                        continue;
                    }
                } else {
                    memcpy(rgb_frame, fb->buf, fb->width * fb->height * 3);
                }
                // Resize to 96x96 for TFLite model
                resizeNearestNeighbor(rgb_frame, fb->width, fb->height,
                                      resized_frame, 96, 96);

                ESP_LOGI(TF_TAG, "First 10 pixels: %u %u %u %u %u ...",
                         resized_frame[0], resized_frame[1], resized_frame[2],
                         resized_frame[3], resized_frame[4]);
                // Log input tensor shape and type
                TfLiteTensor* input = tf_wrapper.getInputTensor(); // Add this method to your wrapper
                if (input) {
                    ESP_LOGI(TF_TAG, "Input tensor type: %d", input->type);
                    ESP_LOGI(TF_TAG, "Input tensor shape: %d x %d x %d",
                             input->dims->data[1], input->dims->data[2], input->dims->data[3]);
                }
                // Run inference
                bool person_present = tf_wrapper.runInference(resized_frame, 96, 96);
                if (person_present) {
                    ESP_LOGI(TF_TAG, "Person detected in frame!");
                } else {
                    ESP_LOGI(TF_TAG, "No person detected");
                }
            }
            esp_camera_fb_return(fb);
        }
        ESP_LOGI(CAPTURE_TAG, "Captured frame: %u bytes", (unsigned)latest_frame.size());
        vTaskDelay(pdMS_TO_TICKS(10000)); // 10 second interval
    }
}

// HTTP handler — returns the latest captured frame
esp_err_t capture_http_handler(httpd_req_t *req)
{
    if (xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    if (latest_frame.empty()) {
        xSemaphoreGive(frame_mutex);
        httpd_resp_send_500(req);
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "image/jpeg");
    esp_err_t res = httpd_resp_send(req,
                                    reinterpret_cast<const char *>(latest_frame.data()),
                                    latest_frame.size());
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

    // --- Wi-Fi initialization ---
    wifi.init();
    wifi.initSTA("FedericoGA35", "chapischapis"); // connect to your network

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

    // --- Initialize TensorFlow Lite wrapper ---
    tf_wrapper = TfLiteWrapper(g_person_detect_model_data, 128 * 1024);

    // --- Wi-Fi initialization ---
    wifi.init();
    wifi.initSTA("FedericoGA35", "chapischapis"); // connect to your network

    // --- HTTP server setup ---
    server.setCaptureHandler(capture_http_handler);

    if (server.start() == ESP_OK) {
        ESP_LOGI(OV2640_TAG, "HTTP Server started. Open http://%s/capture.jpg", wifi.getLocalIP().c_str());
    } else {
        ESP_LOGE(OV2640_TAG, "HTTP Server not started");
    }

    // --- Start capture task ---
    frame_mutex = xSemaphoreCreateMutex();
    if (frame_mutex) {
        xTaskCreate(capture_task, "capture_task", 16384, nullptr, 5, nullptr);
        ESP_LOGI(OV2640_TAG, "Started periodic capture task");
    } else {
        ESP_LOGE(OV2640_TAG, "Failed to create frame mutex");
    }

    // --- Run for 60 seconds, then reboot ---
    const uint32_t run_time_ms = 300000;  // 1 minute
    ESP_LOGD(OV2640_TAG, "System will reboot in %d seconds...", (int)(run_time_ms / 1000));
    vTaskDelay(pdMS_TO_TICKS(run_time_ms));

    ESP_LOGW(OV2640_TAG, "Rebooting system after 1 minute...");
    esp_restart();   // clean software reboot
}