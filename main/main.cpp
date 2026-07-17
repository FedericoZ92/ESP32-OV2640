
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
#include "app-globals.h"
#include "psram/psram.h"
#include "network.h"
#include "checkpoint-timer/checkpoint-timer.h"
#include "util/misc.h"
#include "define.h"
#include "data-types/frame-mailbox.h"
#include "http-server/http-frame-buffer.h"
#include <stdio.h>
#include <cstring>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>
#include <esp_system.h>
#include <esp_log.h>
#include <esp_chip_info.h>
#include <esp_flash.h>
#include <esp_timer.h>
#include <esp_camera.h>
#include <lwip/sockets.h>
#include <lwip/inet.h>
#include <lwip/netdb.h>

// OV2640 pin map for ESP32-S3-CAM-N16R8
// https://www.oceanlabz.in/getting-started-with-esp32-s3-wroom-n16r8-cam-dev-board/?srsltid=AfmBOors-1xeo_-CM5mcneEFHgQY9ps0qX2SHt8gf-S7Ndizot0T4vzk
// in docs: schematics file from: https://www.homotix.it/vendita/moduli-wi-fi/scheda-esp32-s3-n16r8
// https://github.com/microrobotics/ESP32-S3-N16R8/blob/main/ESP32-S3-N16R8_User_Guide.pdf
// https://www.fruugo.it/esp32-s3-wroom-n16r8-modulo-fotocamera-per-scheda-di-sviluppo-cam-con-ov2640/p-358759907-780375612?language=it&ac=google&utm_source=google&utm_medium=paid&gad_source=1&gad_campaignid=22510258486&gbraid=0AAAAADpXug2uMu5_YIv6BL_H9NJZ76oa1&gclid=EAIaIQobChMI8brdxtT0kQMVuZqDBx1AHgjiEAQYASABEgI-qPD_BwE

// camera parameters in camera-driver.cpp!

static CameraHttpServer server;
static WifiManager wifi;
RgbLedController rgb(GPIO_NUM_48, RMT_CHANNEL_0); // 8 LEDs on GPIO48
RedLedController redLed = RedLedController();
static TfLiteWrapper tf_wrapper;

CheckpointTimer jpegTimer;
CheckpointTimer cameraAcquisitionTimer;
CheckpointTimer tensorFlowTimer;
CheckpointTimer httpTimer;

// --- Shared static frame buffer ---
/*static uint8_t* httpFrameBuffers[2] = {nullptr, nullptr};
static volatile uint8_t activeHttpFrameIndex = 0;
static volatile size_t activeHttpFrameLen = 0;
static volatile uint16_t activeHttpFrameWidth = TF_IMAGE_INPUT_SIZE;
static volatile uint16_t activeHttpFrameHeight = TF_IMAGE_INPUT_SIZE;
static volatile pixformat_t activeHttpFrameFormat = PIXFORMAT_GRAYSCALE;
static volatile uint32_t activeHttpFrameSeq = 0;
static volatile int64_t activeHttpFramePublishUs = 0;*/
HttpFrameBuffer httpFrameBuffer(kPublishedFrameMaxBytes);

volatile bool pauseCameraAcquisition = false;

static portMUX_TYPE httpFrameMetaLock = portMUX_INITIALIZER_UNLOCKED;


FrameMailbox inferenceMailbox;
FrameMailbox streamMailbox;
FrameMailboxManager inferenceMailboxManager(&inferenceMailbox, kPublishedFrameMaxBytes);
FrameMailboxManager streamMailboxManager(&streamMailbox, kPublishedFrameMaxBytes);

// HTTP handler, returns the latest captured frame payload, uses httpd_resp_send (HTTP over TCP)
esp_err_t captureRgbTcpCallback(httpd_req_t *req)
{
    const int64_t reqStartUs = esp_timer_get_time();
    static int64_t httpReqWindowStartUs = 0;
    static uint32_t httpReqWindowCount = 0;
    static uint32_t httpReqWindowStale = 0;
    static uint32_t httpReqWindowAdvanceEvents = 0;
    static uint32_t httpReqWindowAdvancedFrames = 0;
    static uint32_t httpReqWindowMaxAdvance = 0;
    static uint32_t httpReqWindowDroppedEstimate = 0;
    static uint32_t httpReqWindowMaxDroppedBurst = 0;
    static uint32_t httpReqWindowLastSeq = 0;

    // Optional debug toggle: /capture.rgb?freeze=1 or /capture.rgb?freeze=0
    char query[64] = {0};
    const size_t queryLen = httpd_req_get_url_query_len(req);
    if (queryLen > 0 && queryLen < sizeof(query)) {
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK) {
            char freezeVal[8] = {0};
            if (httpd_query_key_value(query, "freeze", freezeVal, sizeof(freezeVal)) == ESP_OK) {
                const bool newPauseState = (strcmp(freezeVal, "1") == 0 || strcmp(freezeVal, "true") == 0);
                static bool lastLoggedPauseState = false;
                pauseCameraAcquisition = newPauseState;
                if (newPauseState != lastLoggedPauseState) {
                    ESP_LOGW(CAPTURE_TAG, "Capture freeze mode: %s", newPauseState ? "ON" : "OFF");
                    lastLoggedPauseState = newPauseState;
                }
            }
        }
    }

    httpd_resp_set_type(req, "application/octet-stream");

    size_t frameLength = 0;
    uint8_t frameIndex = 0;
    uint16_t frameWidth = TF_IMAGE_INPUT_SIZE;
    uint16_t frameHeight = TF_IMAGE_INPUT_SIZE;
    pixformat_t frameFormat = PIXFORMAT_GRAYSCALE;
    uint32_t frameSeq = 0;
    int64_t framePublishUs = 0;
    taskENTER_CRITICAL(&httpFrameMetaLock);
    frameLength = httpFrameBuffer.getActiveHttpFrameLength();
    frameIndex = httpFrameBuffer.getActiveHttpFrameIndex();
    frameWidth = httpFrameBuffer.getActiveHttpFrameWidth();
    frameHeight = httpFrameBuffer.getActiveHttpFrameHeight();
    frameFormat = httpFrameBuffer.getActiveHttpFrameFormat();
    frameSeq = httpFrameBuffer.getActiveHttpFrameSeq();
    framePublishUs = httpFrameBuffer.getActiveHttpFramePublishUs();
    taskEXIT_CRITICAL(&httpFrameMetaLock);

    if (frameFormat == PIXFORMAT_JPEG) {
        httpd_resp_set_type(req, "image/jpeg");
    } else {
        httpd_resp_set_type(req, "application/octet-stream");
    }

    // Expose frame telemetry in HTTP headers for browser-side diagnostics.
    char seqBuf[16];
    snprintf(seqBuf, sizeof(seqBuf), "%lu", (unsigned long)frameSeq);
    httpd_resp_set_hdr(req, "X-Frame-Seq", seqBuf);

    const int64_t nowUs = esp_timer_get_time();
    const int64_t ageUs = (framePublishUs > 0 && nowUs > framePublishUs) ? (nowUs - framePublishUs) : 0;
    char ageBuf[24];
    snprintf(ageBuf, sizeof(ageBuf), "%lld", (long long)(ageUs / 1000));
    httpd_resp_set_hdr(req, "X-Frame-Age-Ms", ageBuf);

    char lenBuf[16];
    snprintf(lenBuf, sizeof(lenBuf), "%u", (unsigned int)frameLength);
    httpd_resp_set_hdr(req, "X-Frame-Len", lenBuf);

    char widthBuf[8];
    snprintf(widthBuf, sizeof(widthBuf), "%u", (unsigned int)frameWidth);
    httpd_resp_set_hdr(req, "X-Frame-Width", widthBuf);

    char heightBuf[8];
    snprintf(heightBuf, sizeof(heightBuf), "%u", (unsigned int)frameHeight);
    httpd_resp_set_hdr(req, "X-Frame-Height", heightBuf);

    httpd_resp_set_hdr(req, "X-Frame-Format", (frameFormat == PIXFORMAT_JPEG) ? "jpeg" : "gray8");

    if (httpReqWindowStartUs == 0) {
        httpReqWindowStartUs = nowUs;
    }

    httpReqWindowCount++;
    if (frameSeq == httpReqWindowLastSeq && frameSeq != 0) {
        httpReqWindowStale++;
    } else if (httpReqWindowLastSeq != 0 && frameSeq > httpReqWindowLastSeq) {
        const uint32_t advancedBy = frameSeq - httpReqWindowLastSeq;
        httpReqWindowAdvanceEvents++;
        httpReqWindowAdvancedFrames += advancedBy;
        if (advancedBy > 1) {
            const uint32_t dropped = advancedBy - 1;
            httpReqWindowDroppedEstimate += dropped;
            if (dropped > httpReqWindowMaxDroppedBurst) {
                httpReqWindowMaxDroppedBurst = dropped;
            }
        }
        if (advancedBy > httpReqWindowMaxAdvance) {
            httpReqWindowMaxAdvance = advancedBy;
        }
    }
    httpReqWindowLastSeq = frameSeq;

    const uint8_t *sendPtr = nullptr;
    size_t sendLength = frameLength;
    if (frameLength > 0 && httpFrameBuffer.isBuffersInitialized(frameIndex)) {
        sendPtr = httpFrameBuffer.getBuffer(frameIndex);
    } else {
        // Send a blank frame until the first capture is published.
        static uint8_t blank[kPublishedFrameMaxBytes] = {0};
        sendPtr = blank;
        sendLength = sizeof(blank);
    }

    const int64_t sendStartUs = esp_timer_get_time();
    esp_err_t sendErr = httpd_resp_send(req, (const char*)sendPtr, sendLength);
    const int64_t sendEndUs = esp_timer_get_time();
    const int64_t sendUs = (sendEndUs > sendStartUs) ? (sendEndUs - sendStartUs) : 0;
    const int64_t reqTotalUs = (sendEndUs > reqStartUs) ? (sendEndUs - reqStartUs) : 0;

    static uint32_t txWindowCount = 0;
    static uint32_t txWindowErrCount = 0;
    static int64_t txWindowStartUs = 0;
    static int64_t txWindowSendSumUs = 0;
    static int64_t txWindowReqSumUs = 0;
    static int64_t txWindowSendMaxUs = 0;
    if (txWindowStartUs == 0) {
        txWindowStartUs = sendEndUs;
    }

    txWindowCount++;
    txWindowSendSumUs += sendUs;
    txWindowReqSumUs += reqTotalUs;
    if (sendUs > txWindowSendMaxUs) {
        txWindowSendMaxUs = sendUs;
    }
    if (sendErr != ESP_OK) {
        txWindowErrCount++;
        static int64_t lastSendErrLogUs = 0;
        const int64_t nowErrUs = sendEndUs;
        if (nowErrUs - lastSendErrLogUs >= 1000000) {
            ESP_LOGW("HTTP_TX", "httpd_resp_send failed: %s", esp_err_to_name(sendErr));
            lastSendErrLogUs = nowErrUs;
        }
    }

    const int64_t txElapsedUs = sendEndUs - txWindowStartUs;
    if (txElapsedUs >= 2000000) {
        const float avgSendMs = (float)txWindowSendSumUs / (1000.0f * (float)txWindowCount);
        const float avgReqMs = (float)txWindowReqSumUs / (1000.0f * (float)txWindowCount);
        const float maxSendMs = (float)txWindowSendMaxUs / 1000.0f;
        ESP_LOGI("HTTP_TX", "avg_send_ms=%.2f | max_send_ms=%.2f | avg_req_ms=%.2f | err=%lu/%lu | len=%u | seq=%lu",
                 avgSendMs,
                 maxSendMs,
                 avgReqMs,
                 (unsigned long)txWindowErrCount,
                 (unsigned long)txWindowCount,
                 (unsigned int)sendLength,
                 (unsigned long)frameSeq);
        txWindowCount = 0;
        txWindowErrCount = 0;
        txWindowSendSumUs = 0;
        txWindowReqSumUs = 0;
        txWindowSendMaxUs = 0;
        txWindowStartUs = sendEndUs;
    }

    const int64_t httpReqElapsedUs = sendEndUs - httpReqWindowStartUs;
    if (httpReqElapsedUs >= 2000000) {
        const float reqFps = (httpReqWindowCount * 1000000.0f) / (float)httpReqElapsedUs;
        const float avgAdvance = (httpReqWindowAdvanceEvents > 0)
            ? ((float)httpReqWindowAdvancedFrames / (float)httpReqWindowAdvanceEvents)
            : 0.0f;
        const float droppedPerSec = (httpReqWindowDroppedEstimate * 1000000.0f) / (float)httpReqElapsedUs;
        const float droppedPerReq = (httpReqWindowCount > 0)
            ? ((float)httpReqWindowDroppedEstimate / (float)httpReqWindowCount)
            : 0.0f;
        ESP_LOGI("HTTP_CAPTURE",
                 "req_fps=%.2f | stale=%lu/%lu | dropped_est=%lu | drop_per_req=%.2f | drop_per_sec=%.2f | seq=%lu | age_ms=%lld | len=%u | producer_advance_avg=%.2f | producer_advance_max=%lu",
                 reqFps,
                 (unsigned long)httpReqWindowStale,
                 (unsigned long)httpReqWindowCount,
                 (unsigned long)httpReqWindowDroppedEstimate,
                 droppedPerReq,
                 droppedPerSec,
                 (unsigned long)frameSeq,
                 (long long)(ageUs / 1000),
                 (unsigned int)sendLength,
                 avgAdvance,
                 (unsigned long)httpReqWindowMaxAdvance);

        if (httpReqWindowDroppedEstimate > 0) {
            const float dropRatioPerReq = (httpReqWindowCount > 0)
                ? ((float)httpReqWindowDroppedEstimate / (float)httpReqWindowCount)
                : 0.0f;
            const float dropSeverity = ((httpReqWindowDroppedEstimate + httpReqWindowCount) > 0)
                ? ((float)httpReqWindowDroppedEstimate /
                   (float)(httpReqWindowDroppedEstimate + httpReqWindowCount))
                : 0.0f;
            const bool enoughSamples = (httpReqWindowCount >= 10);
            const bool activeClient = (reqFps >= 3.0f);

            if (!enoughSamples || !activeClient) {
                ESP_LOGW("HTTP_CAPTURE",
                         "FRAME_DROP_STATE: low-sample/backlog window (dropped=%lu, req=%lu, req_fps=%.2f, drop_per_req=%.2f, max_burst=%lu)",
                         (unsigned long)httpReqWindowDroppedEstimate,
                         (unsigned long)httpReqWindowCount,
                         reqFps,
                         dropRatioPerReq,
                         (unsigned long)httpReqWindowMaxDroppedBurst);
            } else if (dropSeverity >= 0.50f || httpReqWindowMaxDroppedBurst >= 5) {
                ESP_LOGE("HTTP_CAPTURE",
                         "FRAME_DROP_STATE: heavy drop detected (dropped=%lu, req=%lu, severity=%.2f, drop_per_req=%.2f, max_burst=%lu)",
                         (unsigned long)httpReqWindowDroppedEstimate,
                         (unsigned long)httpReqWindowCount,
                         dropSeverity,
                         dropRatioPerReq,
                         (unsigned long)httpReqWindowMaxDroppedBurst);
            } else {
                ESP_LOGW("HTTP_CAPTURE",
                         "FRAME_DROP_STATE: drop detected (dropped=%lu, req=%lu, severity=%.2f, drop_per_req=%.2f, max_burst=%lu)",
                         (unsigned long)httpReqWindowDroppedEstimate,
                         (unsigned long)httpReqWindowCount,
                         dropSeverity,
                         dropRatioPerReq,
                         (unsigned long)httpReqWindowMaxDroppedBurst);
            }
        }

        if (httpReqWindowCount > 0 && httpReqWindowStale > (httpReqWindowCount / 2)) {
            ESP_LOGW("HTTP_CAPTURE",
                     "STALE_FRAME_STATE: over 50%% stale responses (%lu/%lu)",
                     (unsigned long)httpReqWindowStale,
                     (unsigned long)httpReqWindowCount);
        }
        httpReqWindowStartUs = sendEndUs;
        httpReqWindowCount = 0;
        httpReqWindowStale = 0;
        httpReqWindowAdvanceEvents = 0;
        httpReqWindowAdvancedFrames = 0;
        httpReqWindowMaxAdvance = 0;
        httpReqWindowDroppedEstimate = 0;
        httpReqWindowMaxDroppedBurst = 0;
    }

    return sendErr;
}

// HTTP handler, keeps one connection open and streams frames continuously, uses httpd_resp_send_chunk (HTTP multipart over TCP)
esp_err_t streamRgbTcpCallback(httpd_req_t *req)
{
    esp_err_t res = ESP_OK;
    char *part_buf = (char*)malloc(256);
    if (!part_buf) {
        ESP_LOGE("HTTP_STREAM", "Failed to allocate header buffer for streaming");
        return ESP_ERR_NO_MEM;
    }

    #define STREAM_BOUNDARY "123456789000000000000987654321"
    
    // 1. Prepare initial Multipart headers
    res = httpd_resp_set_type(req, "multipart/x-mixed-replace;boundary=" STREAM_BOUNDARY);
    if (res != ESP_OK) { free(part_buf); return res; }
    res = httpd_resp_set_hdr(req, "Connection", "keep-alive");
    if (res != ESP_OK) { free(part_buf); return res; }
    res = httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
    if (res != ESP_OK) { free(part_buf); return res; }
    res = httpd_resp_set_hdr(req, "Pragma", "no-cache");
    if (res != ESP_OK) { free(part_buf); return res; }
    res = httpd_resp_set_hdr(req, "Expires", "0");
    if (res != ESP_OK) { free(part_buf); return res; }

    // 2. FORCE-FLUSH HEADERS IMMEDIATELY
    // Sending a tiny, harmless 2-byte chunk (\r\n) triggers the HTTP server 
    // to instantly send the HTTP "200 OK" and headers to the browser.
    // This transitions the browser UI from "Connecting..." to "Streaming active...".
    res = httpd_resp_send_chunk(req, "\r\n", 2);
    if (res != ESP_OK) { free(part_buf); return res; }

    uint32_t lastSeenSeq = 0;
    ESP_LOGI("HTTP_STREAM", "Started real-time multipart stream session");

    // 3. Stream loop
    while (true) {
        size_t frameLength = 0;
        uint8_t frameIndex = 0;
        uint16_t frameWidth = TF_IMAGE_INPUT_SIZE;
        uint16_t frameHeight = TF_IMAGE_INPUT_SIZE;
        pixformat_t frameFormat = PIXFORMAT_GRAYSCALE;
        uint32_t frameSeq = 0;
        int64_t framePublishUs = 0;

        // Check if a new frame is ready
        taskENTER_CRITICAL(&httpFrameMetaLock);
        frameSeq = httpFrameBuffer.getActiveHttpFrameSeq();
        taskEXIT_CRITICAL(&httpFrameMetaLock);

        if (frameSeq == lastSeenSeq) {
            // No new frame yet; yield to give the capture task CPU time
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // Extract metadata safely
        taskENTER_CRITICAL(&httpFrameMetaLock);
        frameLength = httpFrameBuffer.getActiveHttpFrameLength();
        frameIndex = httpFrameBuffer.getActiveHttpFrameIndex();
        frameWidth = httpFrameBuffer.getActiveHttpFrameWidth();
        frameHeight = httpFrameBuffer.getActiveHttpFrameHeight();
        frameFormat = httpFrameBuffer.getActiveHttpFrameFormat();
        framePublishUs = httpFrameBuffer.getActiveHttpFramePublishUs();
        taskEXIT_CRITICAL(&httpFrameMetaLock);

        // Read directly from the active buffer (safe due to double-buffering architecture)
        const uint8_t *sendPtr = httpFrameBuffer.getBuffer(frameIndex);
        if (!sendPtr || frameLength == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        const int64_t nowUs = esp_timer_get_time();
        const int64_t ageUs = (framePublishUs > 0 && nowUs > framePublishUs) ? (nowUs - framePublishUs) : 0;

        // Format multipart headers for this individual frame, including telemetry
        int hlen = snprintf(part_buf, 256,
            "--" STREAM_BOUNDARY "\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "X-Frame-Seq: %lu\r\n"
            "X-Frame-Age-Ms: %lld\r\n"
            "X-Frame-Width: %d\r\n"
            "X-Frame-Height: %d\r\n"
            "X-Frame-Format: %s\r\n"
            "\r\n",
            (frameFormat == PIXFORMAT_JPEG) ? "image/jpeg" : "application/octet-stream",
            (size_t)frameLength,
            (unsigned long)frameSeq,
            (long long)(ageUs / 1000),
            (int)frameWidth,
            (int)frameHeight,
            (frameFormat == PIXFORMAT_JPEG) ? "jpeg" : "gray8"
        );

        // Send boundary and custom headers
        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res != ESP_OK) break;

        // Send binary frame buffer payload
        res = httpd_resp_send_chunk(req, (const char*)sendPtr, frameLen);
        if (res != ESP_OK) break;

        // Terminate part with a carriage return line feed
        res = httpd_resp_send_chunk(req, "\r\n", 2);
        if (res != ESP_OK) break;

        lastSeenSeq = frameSeq;
    }

    ESP_LOGI("HTTP_STREAM", "Multipart stream session ended: %s", esp_err_to_name(res));
    free(part_buf);
    return res;
}

static void operateCameraResetSequence()
{
    gpio_reset_pin((gpio_num_t)PWDN_GPIO_NUM);
    gpio_reset_pin((gpio_num_t)RESET_GPIO_NUM);
    gpio_set_direction((gpio_num_t)PWDN_GPIO_NUM, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)RESET_GPIO_NUM, GPIO_MODE_OUTPUT);

    gpio_set_level((gpio_num_t)PWDN_GPIO_NUM, 0);  // Power on
    gpio_set_level((gpio_num_t)RESET_GPIO_NUM, 0); // Hold reset
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)RESET_GPIO_NUM, 1); // Release reset

}

extern "C" void app_main(void)
{
    esp_log_level_set("*", ESP_LOG_WARN);

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
    operateCameraResetSequence();
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
    esp_err_t wifiInitErr = wifi.init();
    if (wifiInitErr != ESP_OK) {
        ESP_LOGE(OV2640_TAG, "Wi-Fi base init failed: %s", esp_err_to_name(wifiInitErr));
        return;
    }

    esp_err_t wifiStaErr = wifi.initSTA(WIFI_NETWORK, WIFI_PASSWORD); // connect to your network
    if (wifiStaErr != ESP_OK) {
        ESP_LOGE(OV2640_TAG, "Wi-Fi STA connection failed, capture/HTTP startup aborted");
        return;
    }

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

    if (!httpFrameBuffer.initPublishedHttpFrameStore(kPublishedFrameMaxBytes, MALLOC_CAP_SPIRAM)) {
        return;
    }

    #if ENABLE_RGB_STREAM_TASK
        if (!streamMailboxManager.initFrameMailbox("stream", MALLOC_CAP_SPIRAM)) {
            return;
        }
    #endif

    #if ENABLE_INFERENCE
        if (!inferenceMailboxManager.initFrameMailbox("inference", MALLOC_CAP_SPIRAM)) {
            return;
        }
    #endif

    // HTTP server task (always on; transport-specific handlers are configured below).
    auto http_server_task = [](void* arg) {
        #if USE_TCP
            server.setCaptureHandler(captureRgbTcpCallback);
            server.setStreamHandler(streamRgbTcpCallback);
        #endif

        if (server.start() == ESP_OK) {
            #if USE_TCP
                ESP_LOGI(OV2640_TAG, "TCP mode active. HTTP server started at http://%s/", wifi.getLocalIP().c_str());
            #else
                ESP_LOGI(OV2640_TAG, "UDP mode active. HTTP status page available at http://%s/", wifi.getLocalIP().c_str());
            #endif
        } else {
            ESP_LOGE(OV2640_TAG, "HTTP server not started");
        }
        vTaskDelete(NULL); // Clean up task after starting server
    };

    xTaskCreatePinnedToCore(
        http_server_task,
        "http_server_task",
        4096,
        NULL,
        5,
        NULL,
        CORE_ID_HTTP_SERVER
    );

    #if USE_UDP
        ESP_LOGI(OV2640_TAG, "UDP mode active. Stream port: %d", UDP_STREAM_PORT);
        xTaskCreatePinnedToCore(
            [&server](void* arg) { 
                CameraHttpServer* serverPtr = static_cast<CameraHttpServer*>(arg);
                if (serverPtr) {
                    serverPtr->udp_stream_task(arg);
                }
            },
            "udp_stream_task",
            6144,
            &server,
            5,
            NULL,
            0
        );
    #endif

    #if ENABLE_RGB_STREAM_TASK
        xTaskCreatePinnedToCore(
            [&server](void* arg) { 
                CameraHttpServer* serverPtr = static_cast<CameraHttpServer*>(arg);
                if (serverPtr) {
                    serverPtr->http_stream_publish_task(arg);
                }
            },
            "http_stream_publish_task",
            6144,
            &server,
            5,
            &streamMailbox.consumerTaskHandle,
            tskNO_AFFINITY
        );
        ESP_LOGI(OV2640_TAG, "Started stream publish task");
    #else
        ESP_LOGW(OV2640_TAG, "RGB stream publish task disabled");
    #endif

    #if ENABLE_INFERENCE
        xTaskCreatePinnedToCore(
            [&tf_wrapper](void* arg) { 
                TfLiteWrapper* wrapperPtr = static_cast<TfLiteWrapper*>(arg); 
                if(wrapperPtr) {
                    tf_wrapper.inference_task(wrapperPtr);
                }
            },
            "inference_task",
            6144,
            &tf_wrapper,
            5,
            &inferenceMailbox.consumerTaskHandle,
            tskNO_AFFINITY
        );
        ESP_LOGI(OV2640_TAG, "Started inference task");
    #else
        ESP_LOGW(TF_TAG, "Inference task disabled for this build");
    #endif

    // --- Start capture task ---
    #if ENABLE_CAMERA_ACQUISITION_TASK
        xTaskCreatePinnedToCore( 
                                [](void* arg) {
                                    CameraDriver* cameraPtr = static_cast<CameraDriver*>(arg);
                                    if (cameraPtr) {
                                        cameraPtr->capture_task(arg);
                                    }
                                },
                                "capture_task",
                                4096,
                                &camera,
                                5,
                                NULL,
                                1); //xCoreID 0 for main core, 1 for app core
        ESP_LOGI(OV2640_TAG, "Started camera acquisition task");
    #else
        ESP_LOGW(OV2640_TAG, "Camera acquisition task disabled");
    #endif

    // --- Optional: run for a limited time, then reboot ---
    /*char buffer[1024]; 
    for (int i = 0; i < 30; i++) { // log task stats every 5 seconds for 25 seconds
        vTaskGetRunTimeStats(buffer);
        printf("Task Name\tRun Time\tCPU %%\n");
        printf("%s\n", buffer);
        vTaskDelay(pdMS_TO_TICKS(1000 * 10)); //10 sec 
    }*/

    //ESP_LOGW(OV2640_TAG, "Rebooting system...");
    //esp_restart();
}