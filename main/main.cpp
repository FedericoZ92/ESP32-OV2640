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
#include "led/rgb-led.h"
#include "led/red-led.h"
#include "psram/psram.h"
#include "network.h"
#include "checkpoint-timer/checkpoint-timer.h"

// OV2640 pin map for ESP32-S3-CAM-N16R8
// https://www.oceanlabz.in/getting-started-with-esp32-s3-wroom-n16r8-cam-dev-board/?srsltid=AfmBOors-1xeo_-CM5mcneEFHgQY9ps0qX2SHt8gf-S7Ndizot0T4vzk
// in docs: schematics file from: https://www.homotix.it/vendita/moduli-wi-fi/scheda-esp32-s3-n16r8
// https://github.com/microrobotics/ESP32-S3-N16R8/blob/main/ESP32-S3-N16R8_User_Guide.pdf
// https://www.fruugo.it/esp32-s3-wroom-n16r8-modulo-fotocamera-per-scheda-di-sviluppo-cam-con-ov2640/p-358759907-780375612?language=it&ac=google&utm_source=google&utm_medium=paid&gad_source=1&gad_campaignid=22510258486&gbraid=0AAAAADpXug2uMu5_YIv6BL_H9NJZ76oa1&gclid=EAIaIQobChMI8brdxtT0kQMVuZqDBx1AHgjiEAQYASABEgI-qPD_BwE


#define JPEG_BUFFER_SIZE 20 * 1024 // FRAMESIZE_QQVGA: 160x120
#define ARENA_SIZE 128 * 1024;

CameraHttpServer server;
WifiManager wifi;
RgbLedController rgb(GPIO_NUM_48, RMT_CHANNEL_0); // 8 LEDs on GPIO48
    
RedLedController redLed = RedLedController();

CheckpointTimer jpegTimer;
CheckpointTimer cameraAcquisitionTimer;
CheckpointTimer tensorFlowTimer;
CheckpointTimer httpTimer;

// --- Shared static frame buffer ---
static uint8_t *latestFrameForHttpUploading = NULL;
static SemaphoreHandle_t frame_mutex = nullptr;
static TfLiteWrapper tf_wrapper;
static size_t latestFrameForHttpUploading_len = 0; // store size of latest JPEG frame
uint8_t *rawImageBuffer = nullptr; // raw pixels
uint8_t* tflite_input_buffer = nullptr; // default: raw buffer
static uint8_t gray_frame[TF_IMAGE_INPUT_SIZE * TF_IMAGE_INPUT_SIZE];

// Background task: capture a new frame every 10 seconds
void capture_task(void *arg)
{
    ESP_LOGI(CAPTURE_TAG, "Camera capture task started");
    static uint8_t resized_frame[TF_IMAGE_INPUT_SIZE * TF_IMAGE_INPUT_SIZE];  // Grayscale input for TFLite
    const size_t jpeg_buf_size = JPEG_BUFFER_SIZE;
    latestFrameForHttpUploading = (uint8_t*) heap_caps_malloc(jpeg_buf_size, MALLOC_CAP_SPIRAM);
    if (!latestFrameForHttpUploading) {
        ESP_LOGE(CAPTURE_TAG, "Failed to allocate PSRAM buffer for JPEG frames!");
        return;
    }
    latestFrameForHttpUploading_len = 0;

    while (true) {
        // --- Handle JPEG decoding or raw frame ---
        ESP_LOGI(CAPTURE_TAG, "Handle JPEG decoding or raw frame, mark checkpoint"); 
        cameraAcquisitionTimer.checkpoint();
        camera_fb_t *frameBuffer = esp_camera_fb_get();
        if (!frameBuffer) {
            ESP_LOGW(CAPTURE_TAG, "Failed to get frame buffer");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        ESP_LOGI(CAPTURE_TAG, "Frame: %dx%d, len=%d, format=%d", frameBuffer->width, frameBuffer->height, frameBuffer->len, frameBuffer->format);
        cameraAcquisitionTimer.logCheckpoint(CAPTURE_TAG, "frame captured");
        
        // ---

        // --- Handle JPEG decoding to raw frame ---
        ESP_LOGI(CAPTURE_TAG, "Handle JPEG decoding to raw frame"); 
        jpegTimer.checkpoint();
        // Free previous decoded buffer
        if (rawImageBuffer) {
            heap_caps_free(rawImageBuffer);
            rawImageBuffer = nullptr;
        }
        if (frameBuffer->format == PIXFORMAT_JPEG) {
            rawImageBuffer = allocatingDecodeCameraJpeg(frameBuffer, 
                                                        MALLOC_CAP_SPIRAM, 
                                                        JPEG_IMAGE_FORMAT_RGB565,
                                                        JPEG_IMAGE_SCALE_0);
            if (!rawImageBuffer) {
                ESP_LOGE(CAPTURE_TAG, "JPEG decode failed");
                esp_camera_fb_return(frameBuffer);
                continue;
            }
            tflite_input_buffer = rawImageBuffer;
        } else if (frameBuffer->format == PIXFORMAT_GRAYSCALE || frameBuffer->format == PIXFORMAT_RGB565) {
            tflite_input_buffer = frameBuffer->buf;
        } else {
            ESP_LOGW(CAPTURE_TAG, "Unsupported pixel format %d", frameBuffer->format);
            esp_camera_fb_return(frameBuffer);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        jpegTimer.logCheckpoint(JPEG_TAG, "raw to jpeg conversion done");
        // ---

        // --- TensorFlow Lite inference ---
        ESP_LOGI(CAPTURE_TAG, "TensorFlow Lite inference"); 
        tensorFlowTimer.checkpoint();
        if (frameBuffer->width >= TF_IMAGE_INPUT_SIZE && frameBuffer->height >= TF_IMAGE_INPUT_SIZE) {
            int channels = (frameBuffer->format == PIXFORMAT_RGB565 || frameBuffer->format == PIXFORMAT_RGB888) ? 3 : 1;
            // Crop and resize to 96x96
            cropCenter(tflite_input_buffer, frameBuffer->width, frameBuffer->height, resized_frame, TF_IMAGE_INPUT_SIZE, TF_IMAGE_INPUT_SIZE, channels);
    
            if (channels == 3) { // Convert to grayscale if needed
                convertRgb888ToGrayscale(resized_frame, gray_frame, TF_IMAGE_INPUT_SIZE, TF_IMAGE_INPUT_SIZE);
            } else {
                memcpy(gray_frame, resized_frame, TF_IMAGE_INPUT_SIZE * TF_IMAGE_INPUT_SIZE);
            }
            // Run inference
            TfLiteTensor* input = tf_wrapper.getInputTensor();
            if (input && input->dims && input->dims->size >= 4) {
                bool person_present = tf_wrapper.runInference(gray_frame, TF_IMAGE_INPUT_SIZE, TF_IMAGE_INPUT_SIZE);
                ESP_LOGW(TF_TAG, "Person detected? %s", person_present ? "YES" : "NO");
                if (person_present) {
                    redLed.setLedGpio2(0);
                    rgb.turnBlueLedOn(); // blue

                } else {
                    redLed.setLedGpio2(1);
                    rgb.turnRedLedOn(); // red

                }
            } else {
                ESP_LOGE(TF_TAG, "Input tensor is null or malformed");
            }
        } else {
            ESP_LOGW(CAPTURE_TAG, "Frame too small to resize to 96x96");
        }
        tensorFlowTimer.logCheckpoint(TF_TAG, "tf inference done");
        // ---

        // --- Copy JPEG frame for HTTP server ---
        ESP_LOGI(CAPTURE_TAG, "Copy JPEG frame for HTTP server, ip: %s", wifi.getLocalIP().c_str()); 
        httpTimer.checkpoint();
        if (frameBuffer->format == PIXFORMAT_JPEG) {
            if (xSemaphoreTake(frame_mutex, pdMS_TO_TICKS(500)) == pdTRUE) {
                if (frameBuffer->len <= jpeg_buf_size) {
                    memcpy(latestFrameForHttpUploading, frameBuffer->buf, frameBuffer->len);
                    latestFrameForHttpUploading_len = frameBuffer->len;
                } else {
                    ESP_LOGW(CAPTURE_TAG, "JPEG frame too large for buffer! len=%d", frameBuffer->len);
                }
                xSemaphoreGive(frame_mutex);
            } else {
                ESP_LOGW(CAPTURE_TAG, "Failed to acquire frame mutex for HTTP frame");
            }
        }
        httpTimer.logCheckpoint(HTTP_TAG, "frame uploading done");
        // ---

        esp_camera_fb_return(frameBuffer);

        ESP_LOGI(CAPTURE_TAG, "Wait 5s"); 
        vTaskDelay(pdMS_TO_TICKS(5000));
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
                                    reinterpret_cast<const char *>(latestFrameForHttpUploading),
                                    latestFrameForHttpUploading_len); // use actual frame size
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

    if (!PSRAM::isAvailable()) {
        ESP_LOGD(MAIN_TAG, "PSRAM NOT available.\n");
        return;
    }

    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        ESP_LOGE(OV2640_TAG, "Get flash size failed");
        return;
    }
    ESP_LOGI(OV2640_TAG, "Flash size: %" PRIu32 " MB", flash_size / (1024 * 1024));

    // --- Camera reset sequence ---
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
    const size_t arena_size = ARENA_SIZE; 
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