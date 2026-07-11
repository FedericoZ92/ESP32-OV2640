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
#include "util/misc.h"
#include "define.h"

// OV2640 pin map for ESP32-S3-CAM-N16R8
// https://www.oceanlabz.in/getting-started-with-esp32-s3-wroom-n16r8-cam-dev-board/?srsltid=AfmBOors-1xeo_-CM5mcneEFHgQY9ps0qX2SHt8gf-S7Ndizot0T4vzk
// in docs: schematics file from: https://www.homotix.it/vendita/moduli-wi-fi/scheda-esp32-s3-n16r8
// https://github.com/microrobotics/ESP32-S3-N16R8/blob/main/ESP32-S3-N16R8_User_Guide.pdf
// https://www.fruugo.it/esp32-s3-wroom-n16r8-modulo-fotocamera-per-scheda-di-sviluppo-cam-con-ov2640/p-358759907-780375612?language=it&ac=google&utm_source=google&utm_medium=paid&gad_source=1&gad_campaignid=22510258486&gbraid=0AAAAADpXug2uMu5_YIv6BL_H9NJZ76oa1&gclid=EAIaIQobChMI8brdxtT0kQMVuZqDBx1AHgjiEAQYASABEgI-qPD_BwE

// camera parameters in camera-driver.cpp!

CameraHttpServer server;
WifiManager wifi;
RgbLedController rgb(GPIO_NUM_48, RMT_CHANNEL_0); // 8 LEDs on GPIO48
    
RedLedController redLed = RedLedController();

CheckpointTimer jpegTimer;
CheckpointTimer cameraAcquisitionTimer;
CheckpointTimer tensorFlowTimer;
CheckpointTimer httpTimer;

// --- Shared static frame buffer ---
static uint8_t* httpFrameBuffers[2] = {nullptr, nullptr};
static volatile uint8_t activeHttpFrameIndex = 0;
static volatile size_t activeHttpFrameLen = 0;
static portMUX_TYPE httpFrameMetaLock = portMUX_INITIALIZER_UNLOCKED;
static TfLiteWrapper tf_wrapper;
uint8_t *rawImageBuffer = nullptr; // raw pixels
uint8_t* tflitePreEditingBuffer = nullptr; // default: raw buffer
static uint8_t tfliteGray96x96InputBuffer[TF_IMAGE_INPUT_SIZE * TF_IMAGE_INPUT_SIZE];

// Background task: capture and process frames continuously
void capture_task(void *arg)
{
    ESP_LOGI(CAPTURE_TAG, "Camera capture task started");
    static uint8_t processedFrame[TF_IMAGE_INPUT_SIZE * TF_IMAGE_INPUT_SIZE];
    const size_t frameBufferSize = TF_IMAGE_INPUT_SIZE * TF_IMAGE_INPUT_SIZE;
    httpFrameBuffers[0] = (uint8_t*) heap_caps_malloc(frameBufferSize, MALLOC_CAP_SPIRAM);
    httpFrameBuffers[1] = (uint8_t*) heap_caps_malloc(frameBufferSize, MALLOC_CAP_SPIRAM);
    if (!httpFrameBuffers[0] || !httpFrameBuffers[1]) {
        ESP_LOGE(CAPTURE_TAG, "Failed to allocate PSRAM buffer for JPEG frames!");
        return;
    }
    activeHttpFrameLen = 0;

    while (true) {
        // --- Handle JPEG decoding or raw frame ---
        ESP_LOGD(CAPTURE_TAG, "Handle JPEG decoding or raw frame, mark checkpoint"); 
        cameraAcquisitionTimer.checkpoint();
        camera_fb_t *frameBuffer = esp_camera_fb_get();
        if (!frameBuffer) {
            ESP_LOGW(CAPTURE_TAG, "Failed to get frame buffer");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        ESP_LOGD(CAPTURE_TAG, "Frame: %dx%d, len=%d, format=%d", frameBuffer->width, frameBuffer->height, frameBuffer->len, frameBuffer->format);
        cameraAcquisitionTimer.logCheckpoint(CAPTURE_TAG, "frame captured");
    
        // ---

        // --- Handle JPEG decoding to raw frame ---
        ESP_LOGD(CAPTURE_TAG, "Handle JPEG decoding to raw frame"); 
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
            tflitePreEditingBuffer = rawImageBuffer;
        } else if (frameBuffer->format == PIXFORMAT_GRAYSCALE) {
            tflitePreEditingBuffer = frameBuffer->buf;
        } else {
            ESP_LOGW(CAPTURE_TAG, "Unsupported pixel format %d", frameBuffer->format);
            esp_camera_fb_return(frameBuffer);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        jpegTimer.logCheckpoint(JPEG_TAG, "raw to jpeg conversion done");
        // ---

        // --- TensorFlow Lite inference ---
        ESP_LOGD(CAPTURE_TAG, "TensorFlow Lite inference"); 
        tensorFlowTimer.checkpoint();
        if (frameBuffer->width >= TF_IMAGE_INPUT_SIZE && frameBuffer->height >= TF_IMAGE_INPUT_SIZE) {
            int channels = (frameBuffer->format == PIXFORMAT_RGB565 || frameBuffer->format == PIXFORMAT_RGB888) ? 3 : 1;
                
            if (channels == 3) { // Convert to grayscale if needed
                if (frameBuffer->format == PIXFORMAT_RGB565){
                    convertRgb565ToGrayscale((uint16_t*)tflitePreEditingBuffer, processedFrame, frameBuffer->width, frameBuffer->height);
                } else if (frameBuffer->format == PIXFORMAT_RGB888){
                    convertRgb888ToGrayscale(tflitePreEditingBuffer, processedFrame, frameBuffer->width, frameBuffer->height);
                } else {
                    ESP_LOGW(CAPTURE_TAG, "Unsupported JPEG FORMAT) %d for RGB to grayscale conversion", frameBuffer->format);
                }
                // Crop and resize to 96x96
                cropCenter(processedFrame, frameBuffer->width, frameBuffer->height, tfliteGray96x96InputBuffer, TF_IMAGE_INPUT_SIZE, TF_IMAGE_INPUT_SIZE, 1);
                
            } else {
                // Crop and resize to 96x96
                cropCenter(tflitePreEditingBuffer, frameBuffer->width, frameBuffer->height, processedFrame, TF_IMAGE_INPUT_SIZE, TF_IMAGE_INPUT_SIZE, 1);

                memcpy(tfliteGray96x96InputBuffer, processedFrame, TF_IMAGE_INPUT_SIZE * TF_IMAGE_INPUT_SIZE);
            }

            
            // Publish latest grayscale frame first so HTTP is not blocked by inference latency.
            const int gray_size = TF_IMAGE_INPUT_SIZE * TF_IMAGE_INPUT_SIZE; // 96x96
            uint8_t publishIndex = activeHttpFrameIndex ^ 1;
            if (httpFrameBuffers[publishIndex]) {
                memcpy(httpFrameBuffers[publishIndex], tfliteGray96x96InputBuffer, gray_size);
                taskENTER_CRITICAL(&httpFrameMetaLock);
                activeHttpFrameLen = gray_size;
                activeHttpFrameIndex = publishIndex;
                taskEXIT_CRITICAL(&httpFrameMetaLock);
            }

            // Run inference
#if ENABLE_INFERENCE
            TfLiteTensor* input = tf_wrapper.getInputTensor();
            if (input && input->dims && input->dims->size >= 4) {
                logFirstPixels(TF_TAG, "tfliteGray96x96InputBuffer", tfliteGray96x96InputBuffer, 10);
                bool person_present = tf_wrapper.runInference(tfliteGray96x96InputBuffer, TF_IMAGE_INPUT_SIZE, TF_IMAGE_INPUT_SIZE);
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
#endif
        } else {
            ESP_LOGW(CAPTURE_TAG, "Frame too small to resize to 96x96");
        }
        tensorFlowTimer.logCheckpoint(TF_TAG, "tf inference done");
        // ---

        esp_camera_fb_return(frameBuffer);

        // Yield to networking/HTTP tasks to avoid burst-and-freeze behavior.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

// HTTP handler — returns the latest captured grayscale frame
esp_err_t captureRgbCallback(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/octet-stream");

    size_t frameLen = 0;
    uint8_t frameIndex = 0;
    taskENTER_CRITICAL(&httpFrameMetaLock);
    frameLen = activeHttpFrameLen;
    frameIndex = activeHttpFrameIndex;
    taskEXIT_CRITICAL(&httpFrameMetaLock);

    if (frameLen > 0 && httpFrameBuffers[frameIndex]) {
        httpd_resp_send(req, (const char*)httpFrameBuffers[frameIndex], frameLen);
    } else {
        // Send a blank frame until the first capture is published.
        static uint8_t blank[TF_IMAGE_INPUT_SIZE * TF_IMAGE_INPUT_SIZE] = {0};
        httpd_resp_send(req, (const char*)blank, sizeof(blank));
    }

    return ESP_OK;
}

// HTTP handler — keeps one connection open and streams grayscale frames.
esp_err_t streamRgbCallback(httpd_req_t *req)
{
    static constexpr size_t frameSize = TF_IMAGE_INPUT_SIZE * TF_IMAGE_INPUT_SIZE;
    static uint8_t blank[frameSize] = {0};
#if !STREAM_KEEP_OPEN
    static constexpr int maxFramesPerRequest = 8;
#endif

    httpd_resp_set_type(req, "application/octet-stream");
 
#if STREAM_KEEP_OPEN
    httpd_resp_set_hdr(req, "Connection", "keep-alive");
    while (true) {
#else
    httpd_resp_set_hdr(req, "Connection", "close");
    for (int frameCount = 0; frameCount < maxFramesPerRequest; ++frameCount) {
#endif
        size_t frameLen = 0;
        uint8_t frameIndex = 0;

        taskENTER_CRITICAL(&httpFrameMetaLock);
        frameLen = activeHttpFrameLen;
        frameIndex = activeHttpFrameIndex;
        taskEXIT_CRITICAL(&httpFrameMetaLock);

        const uint8_t *framePtr = nullptr;
        size_t sendLen = frameSize;
        if (frameLen >= frameSize && httpFrameBuffers[frameIndex]) {
            framePtr = httpFrameBuffers[frameIndex];
        } else {
            framePtr = blank;
        }

        esp_err_t err = httpd_resp_send_chunk(req, (const char*)framePtr, sendLen);
        if (err != ESP_OK) {
            return err;
        }

        // Keep stream near the page's target refresh rate.
        vTaskDelay(pdMS_TO_TICKS(250));
    }

#if STREAM_KEEP_OPEN
    // Keep-open mode exits only if send_chunk fails and returns above.
    return ESP_OK;
#else
    // End this chunked response so the browser reconnect loop can continue.
    return httpd_resp_send_chunk(req, nullptr, 0);
#endif
}

extern "C" void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_INFO);

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
#if ENABLE_INFERENCE
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
    if (!tf_wrapper.init(g_person_detect_model_data, arena_size, tensor_arena)) {
        ESP_LOGE(TF_TAG, "TfLite initialization failed!");
        return;
    }
#else
    ESP_LOGW(TF_TAG, "Inference disabled for test build (ENABLE_INFERENCE=0)");
#endif

    log_RAM_status("pre Wi-Fi initialization"); // TODO restore

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

    // HTTP server task 
    auto http_server_task = [](void* arg) {
        server.setCaptureHandler(captureRgbCallback);
        server.setStreamHandler(streamRgbCallback);
        if (server.start() == ESP_OK) {
            ESP_LOGI(OV2640_TAG, "HTTP Server started. Open http://%s/", wifi.getLocalIP().c_str());
        } else {
            ESP_LOGE(OV2640_TAG, "HTTP Server not started");
        }
        vTaskDelete(NULL); // Clean up task after starting server
    };

    // Pass the lambda directly to FreeRTOS
    xTaskCreatePinnedToCore(
        http_server_task,     // The compiler automatically converts this to a function pointer
        "http_server_task",
        4096,
        NULL,                 // Pass NULL since your lambda doesn't use the 'arg' parameter
        5,
        NULL,
        0                     // Pin to core 0 
    );

    // --- Start capture task ---
    xTaskCreatePinnedToCore(capture_task, 
                            "capture_task", 
                            4096, 
                            NULL, 
                            5, 
                            NULL, 
                            1); //xCoreID 0 for main core, 1 for app core
    ESP_LOGI(OV2640_TAG, "Started periodic capture task");

    // --- Optional: run for a limited time, then reboot ---
    char buffer[1024]; 
    for (int i = 0; i < 30; i++) { // log task stats every 5 seconds for 25 seconds
        vTaskGetRunTimeStats(buffer);
        printf("Task Name\tRun Time\tCPU %%\n");
        printf("%s\n", buffer);
        vTaskDelay(pdMS_TO_TICKS(1000 * 10)); //10 sec 
    }

    ESP_LOGW(OV2640_TAG, "Rebooting system...");
    esp_restart();
}