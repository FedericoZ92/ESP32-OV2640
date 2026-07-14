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
static volatile uint32_t activeHttpFrameSeq = 0;
static volatile int64_t activeHttpFramePublishUs = 0;
static volatile bool pauseCameraAcquisition = false;
static portMUX_TYPE httpFrameMetaLock = portMUX_INITIALIZER_UNLOCKED;
static TfLiteWrapper tf_wrapper;
uint8_t *rawImageBuffer = nullptr; // raw pixels
uint8_t* tflitePreEditingBuffer = nullptr; // default: raw buffer
static uint8_t tfliteGray96x96InputBuffer[TF_IMAGE_INPUT_SIZE * TF_IMAGE_INPUT_SIZE];

// Background task: capture and process frames continuously
void capture_task(void *arg)
{
    ESP_LOGI(CAPTURE_TAG, "Camera capture task started");
    uint32_t producedFrames = 0;
    int64_t producerWindowStartUs = esp_timer_get_time();
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
        if (pauseCameraAcquisition) {
            // Freeze mode keeps serving the last published frame to isolate HTTP/network speed.
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

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
                const int64_t nowUs = esp_timer_get_time();
                taskENTER_CRITICAL(&httpFrameMetaLock);
                activeHttpFrameLen = gray_size;
                activeHttpFrameIndex = publishIndex;
                activeHttpFrameSeq++;
                activeHttpFramePublishUs = nowUs;
                taskEXIT_CRITICAL(&httpFrameMetaLock);

                producedFrames++;
                const int64_t elapsedUs = nowUs - producerWindowStartUs;
                if (elapsedUs >= 2000000) {
                    const float producerFps = (producedFrames * 1000000.0f) / (float)elapsedUs;
                    ESP_LOGI(CAPTURE_TAG, "Producer FPS: %.2f | latest seq=%lu | frame=%dx%d fmt=%d len=%d",
                             producerFps,
                             (unsigned long)activeHttpFrameSeq,
                             frameBuffer->width,
                             frameBuffer->height,
                             frameBuffer->format,
                             frameBuffer->len);
                    producedFrames = 0;
                    producerWindowStartUs = nowUs;
                }

                #if STOP_ACQUISITION_AFTER_1_FRAME
                    if (activeHttpFrameSeq == 1) {
                        pauseCameraAcquisition = true;
                        ESP_LOGW(CAPTURE_TAG, "STOP_ACQUISITION_AFTER_1_FRAME active: acquisition paused after first frame");
                    }
                #endif
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

    size_t frameLen = 0;
    uint8_t frameIndex = 0;
    uint32_t frameSeq = 0;
    int64_t framePublishUs = 0;
    taskENTER_CRITICAL(&httpFrameMetaLock);
    frameLen = activeHttpFrameLen;
    frameIndex = activeHttpFrameIndex;
    frameSeq = activeHttpFrameSeq;
    framePublishUs = activeHttpFramePublishUs;
    taskEXIT_CRITICAL(&httpFrameMetaLock);

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
    snprintf(lenBuf, sizeof(lenBuf), "%u", (unsigned int)frameLen);
    httpd_resp_set_hdr(req, "X-Frame-Len", lenBuf);

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
    size_t sendLen = frameLen;
    if (frameLen > 0 && httpFrameBuffers[frameIndex]) {
        sendPtr = httpFrameBuffers[frameIndex];
    } else {
        // Send a blank frame until the first capture is published.
        static uint8_t blank[TF_IMAGE_INPUT_SIZE * TF_IMAGE_INPUT_SIZE] = {0};
        sendPtr = blank;
        sendLen = sizeof(blank);
    }

    const int64_t sendStartUs = esp_timer_get_time();
    esp_err_t sendErr = httpd_resp_send(req, (const char*)sendPtr, sendLen);
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
                 (unsigned int)sendLen,
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
                 (unsigned int)sendLen,
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

// HTTP handler — keeps one connection open and streams grayscale frames.
esp_err_t streamRgbCallback(httpd_req_t *req)
{
    // Keep /stream.rgb compatible but non-blocking: return one frame per request.
    // This prevents long-lived stream requests from starving other HTTP handlers.
    return captureRgbCallback(req);
}

typedef struct __attribute__((packed)) {
    uint32_t magic;
    uint32_t frameSeq;
    uint16_t chunkIndex;
    uint16_t chunkCount;
    uint16_t payloadLen;
    uint16_t frameLen;
    uint32_t frameAgeMs;
} UdpFrameHeader;

void udp_stream_task(void *arg)
{
    const uint32_t kMagic = 0x47504455U; // 'UDPG'
    const int kMaxPayload = UDP_STREAM_MAX_PAYLOAD;
    const int kPort = UDP_STREAM_PORT;

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
    if (sock < 0) {
        ESP_LOGE("UDP_STREAM", "Failed to create UDP socket");
        vTaskDelete(NULL);
        return;
    }

    struct sockaddr_in localAddr = {};
    localAddr.sin_family = AF_INET;
    localAddr.sin_addr.s_addr = htonl(INADDR_ANY);
    localAddr.sin_port = htons((uint16_t)kPort);

    if (bind(sock, (struct sockaddr *)&localAddr, sizeof(localAddr)) != 0) {
        ESP_LOGE("UDP_STREAM", "Failed to bind UDP socket on port %d", kPort);
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    struct timeval recvTimeout = {};
    recvTimeout.tv_sec = 0;
    recvTimeout.tv_usec = 200000;
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &recvTimeout, sizeof(recvTimeout));

    ESP_LOGI("UDP_STREAM", "UDP stream waiting for client hello on port %d", kPort);

    bool hasClient = false;
    struct sockaddr_in clientAddr = {};
    socklen_t clientAddrLen = sizeof(clientAddr);

    uint8_t *frameSnapshot = (uint8_t *)heap_caps_malloc(TF_IMAGE_INPUT_SIZE * TF_IMAGE_INPUT_SIZE, MALLOC_CAP_8BIT);
    if (!frameSnapshot) {
        ESP_LOGE("UDP_STREAM", "Failed to allocate frame snapshot buffer");
        close(sock);
        vTaskDelete(NULL);
        return;
    }

    uint8_t packet[sizeof(UdpFrameHeader) + UDP_STREAM_MAX_PAYLOAD];
    uint32_t lastSentSeq = 0;
    uint32_t txFrames = 0;
    uint32_t txChunks = 0;
    uint32_t txErrors = 0;
    int64_t txWindowStartUs = esp_timer_get_time();

    while (true) {
        char helloBuf[64];
        struct sockaddr_in rxAddr = {};
        socklen_t rxLen = sizeof(rxAddr);
        int rx = recvfrom(sock, helloBuf, sizeof(helloBuf) - 1, 0, (struct sockaddr *)&rxAddr, &rxLen);
        if (rx > 0) {
            helloBuf[rx] = '\0';
            clientAddr = rxAddr;
            clientAddrLen = rxLen;
            hasClient = true;
            ESP_LOGI("UDP_STREAM", "Client registered: %s:%u (%s)",
                     inet_ntoa(clientAddr.sin_addr),
                     ntohs(clientAddr.sin_port),
                     helloBuf);
        }

        if (!hasClient) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }

        size_t frameLen = 0;
        uint8_t frameIndex = 0;
        uint32_t frameSeq = 0;
        int64_t framePublishUs = 0;
        taskENTER_CRITICAL(&httpFrameMetaLock);
        frameLen = activeHttpFrameLen;
        frameIndex = activeHttpFrameIndex;
        frameSeq = activeHttpFrameSeq;
        framePublishUs = activeHttpFramePublishUs;
        taskEXIT_CRITICAL(&httpFrameMetaLock);

        if (frameLen == 0 || !httpFrameBuffers[frameIndex]) {
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        if (frameLen > (TF_IMAGE_INPUT_SIZE * TF_IMAGE_INPUT_SIZE)) {
            frameLen = TF_IMAGE_INPUT_SIZE * TF_IMAGE_INPUT_SIZE;
        }

        if (frameSeq == lastSentSeq) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        memcpy(frameSnapshot, httpFrameBuffers[frameIndex], frameLen);

        const uint16_t chunkCount = (uint16_t)((frameLen + kMaxPayload - 1) / kMaxPayload);
        const int64_t nowUs = esp_timer_get_time();
        const uint32_t frameAgeMs = (framePublishUs > 0 && nowUs > framePublishUs)
            ? (uint32_t)((nowUs - framePublishUs) / 1000)
            : 0;

        bool frameFailed = false;
        for (uint16_t chunkIndex = 0; chunkIndex < chunkCount; chunkIndex++) {
            const size_t offset = chunkIndex * kMaxPayload;
            const size_t remaining = frameLen - offset;
            const uint16_t payloadLen = (uint16_t)((remaining > (size_t)kMaxPayload) ? kMaxPayload : remaining);

            UdpFrameHeader header = {};
            header.magic = htonl(kMagic);
            header.frameSeq = htonl(frameSeq);
            header.chunkIndex = htons(chunkIndex);
            header.chunkCount = htons(chunkCount);
            header.payloadLen = htons(payloadLen);
            header.frameLen = htons((uint16_t)frameLen);
            header.frameAgeMs = htonl(frameAgeMs);

            memcpy(packet, &header, sizeof(header));
            memcpy(packet + sizeof(header), frameSnapshot + offset, payloadLen);

            int sent = sendto(sock,
                              packet,
                              sizeof(header) + payloadLen,
                              0,
                              (struct sockaddr *)&clientAddr,
                              clientAddrLen);
            if (sent < 0) {
                txErrors++;
                frameFailed = true;
                break;
            }
            txChunks++;
        }

        if (!frameFailed) {
            lastSentSeq = frameSeq;
            txFrames++;
        }

        const int64_t elapsedUs = nowUs - txWindowStartUs;
        if (elapsedUs >= 2000000) {
            const float fps = (txFrames * 1000000.0f) / (float)elapsedUs;
            ESP_LOGI("UDP_STREAM",
                     "tx_fps=%.2f | frames=%lu | chunks=%lu | err=%lu | last_seq=%lu | frame_len=%u",
                     fps,
                     (unsigned long)txFrames,
                     (unsigned long)txChunks,
                     (unsigned long)txErrors,
                     (unsigned long)lastSentSeq,
                     (unsigned int)frameLen);
            txWindowStartUs = nowUs;
            txFrames = 0;
            txChunks = 0;
            txErrors = 0;
        }

        vTaskDelay(pdMS_TO_TICKS(1));
    }
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

    // HTTP server task (always on; transport-specific handlers are configured below).
    auto http_server_task = [](void* arg) {
        #if USE_TCP
            server.setCaptureHandler(captureRgbCallback);
            server.setStreamHandler(streamRgbCallback);
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
        0
    );

    #if USE_UDP
        ESP_LOGI(OV2640_TAG, "UDP mode active. Stream port: %d", UDP_STREAM_PORT);
        xTaskCreatePinnedToCore(
            udp_stream_task,
            "udp_stream_task",
            6144,
            NULL,
            5,
            NULL,
            0
        );
    #endif

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