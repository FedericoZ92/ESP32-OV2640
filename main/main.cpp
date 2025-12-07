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
//#include "esp_jpeg/esp_jpeg.h" // ESP-IDF JPEG decode API
#include "network.h"

CameraHttpServer server;
WifiManager wifi;
LedController led;

// --- Shared static frame buffer ---
uint8_t *latest_frame = NULL;
static SemaphoreHandle_t frame_mutex = nullptr;
static TfLiteWrapper tf_wrapper;
static size_t latest_frame_len = 0; // store size of latest JPEG frame
uint8_t *decoded_buffer = nullptr; // raw pixels
uint8_t* tflite_input_buffer = nullptr; // default: raw buffer

// Background task: capture a new frame every 10 seconds
void capture_task(void *arg)
{
    ESP_LOGI(CAPTURE_TAG, "Camera capture task started");

    if (!PSRAM::isAvailable()) {
        ESP_LOGD(MAIN_TAG, "PSRAM NOT available.\n");
        return;
    }

    const size_t jpeg_buf_size = 20 * 1024; 
    latest_frame = (uint8_t*) heap_caps_malloc(jpeg_buf_size, MALLOC_CAP_SPIRAM);
    if (!latest_frame) {
        ESP_LOGE(CAPTURE_TAG, "Failed to allocate PSRAM buffer for JPEG frames!");
        return;
    }
    latest_frame_len = 0;

    while (true) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            ESP_LOGW(CAPTURE_TAG, "Failed to get frame buffer");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        ESP_LOGI(CAPTURE_TAG, "Frame: %dx%d, len=%d, format=%d", fb->width, fb->height, fb->len, fb->format);

        // Free previous decoded buffer
        if (decoded_buffer) {
            heap_caps_free(decoded_buffer);
            decoded_buffer = nullptr;
        }

        if (fb->format == PIXFORMAT_JPEG) {
            // Allocate decoded buffer for RGB565
            decoded_buffer = (uint8_t*) heap_caps_malloc(fb->width * fb->height * 2, MALLOC_CAP_SPIRAM);
            if (!decoded_buffer) {
                ESP_LOGE(CAPTURE_TAG, "Failed to allocate decoded buffer for JPEG");
                esp_camera_fb_return(fb);
                vTaskDelete(NULL);
            }

            esp_jpeg_image_cfg_t cfg = {};
            cfg.indata = fb->buf;
            cfg.indata_size = fb->len;
            cfg.outbuf = decoded_buffer;
            cfg.outbuf_size = fb->width * fb->height * 2;
            cfg.out_format = JPEG_IMAGE_FORMAT_RGB565;
            cfg.out_scale = JPEG_IMAGE_SCALE_0;

            esp_jpeg_image_output_t output_info = {};
            esp_err_t err = esp_jpeg_decode(&cfg, &output_info);
            if (err != ESP_OK) {
                ESP_LOGE(CAPTURE_TAG, "Failed to decode JPEG: %d", err);
                heap_caps_free(decoded_buffer);
                decoded_buffer = nullptr;
                esp_camera_fb_return(fb);
                vTaskDelete(NULL);
            }

            tflite_input_buffer = decoded_buffer;
        } else if (fb->format == PIXFORMAT_GRAYSCALE || fb->format == PIXFORMAT_RGB565) {
            // Already raw pixels — no decoding needed
            tflite_input_buffer = fb->buf;
        } else {
            ESP_LOGW(CAPTURE_TAG, "Unsupported pixel format %d", fb->format);
            esp_camera_fb_return(fb);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        // Copy JPEG frame for HTTP server if needed
        if (fb->format == PIXFORMAT_JPEG) {
            if (xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                if (fb->len <= jpeg_buf_size) {
                    memcpy(latest_frame, fb->buf, fb->len);
                    latest_frame_len = fb->len;
                } else {
                    ESP_LOGW(CAPTURE_TAG, "JPEG frame too large for buffer! len=%d", fb->len);
                }
                xSemaphoreGive(frame_mutex);
            } else {
                ESP_LOGW(CAPTURE_TAG, "Failed to acquire frame mutex for HTTP frame");
            }
        }

        esp_camera_fb_return(fb); // Return the camera framebuffer
        vTaskDelay(pdMS_TO_TICKS(10000)); // Wait 10 seconds before next capture
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
                                    latest_frame_len); // use actual frame size
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