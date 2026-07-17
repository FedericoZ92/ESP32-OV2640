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
#include "app-globals.h"
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
static volatile uint16_t activeHttpFrameWidth = TF_IMAGE_INPUT_SIZE;
static volatile uint16_t activeHttpFrameHeight = TF_IMAGE_INPUT_SIZE;
static volatile pixformat_t activeHttpFrameFormat = PIXFORMAT_GRAYSCALE;
static volatile uint32_t activeHttpFrameSeq = 0;
static volatile int64_t activeHttpFramePublishUs = 0;
static volatile bool pauseCameraAcquisition = false;
static portMUX_TYPE httpFrameMetaLock = portMUX_INITIALIZER_UNLOCKED;
static TfLiteWrapper tf_wrapper;

// Large enough for QQVGA grayscale and worst-case QQVGA JPEG payload bursts.
static constexpr size_t kPublishedFrameMaxBytes =
    (JPEG_BUFFER_SIZE > (160 * 120 * 2) ? JPEG_BUFFER_SIZE : (160 * 120 * 2));

// The current inference camera mode is QQVGA, so a full grayscale scratch frame fits here.
static constexpr size_t kAcquiredFrameMaxPixels = 160 * 120;

struct FrameMailbox {
    uint8_t* buffers[2] = {nullptr, nullptr};
    volatile uint8_t activeIndex = 0;
    volatile size_t frameLen = 0;
    volatile uint16_t frameWidth = 0;
    volatile uint16_t frameHeight = 0;
    volatile pixformat_t frameFormat = PIXFORMAT_GRAYSCALE;
    volatile uint32_t frameSeq = 0;
    volatile int64_t frameCaptureUs = 0;
    portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;
    TaskHandle_t consumerTaskHandle = nullptr;
    const char* tag = nullptr;
};

struct FrameSnapshot {
    const uint8_t* data = nullptr;
    size_t len = 0;
    uint16_t width = 0;
    uint16_t height = 0;
    pixformat_t format = PIXFORMAT_GRAYSCALE;
    uint32_t seq = 0;
    int64_t captureUs = 0;
};

static FrameMailbox inferenceMailbox;
static FrameMailbox streamMailbox;

static bool initFrameMailbox(FrameMailbox* mailbox, const char* tag)
{
    if (!mailbox) {
        return false;
    }

    mailbox->tag = tag;
    mailbox->buffers[0] = (uint8_t*)heap_caps_malloc(kPublishedFrameMaxBytes, MALLOC_CAP_SPIRAM);
    mailbox->buffers[1] = (uint8_t*)heap_caps_malloc(kPublishedFrameMaxBytes, MALLOC_CAP_SPIRAM);
    if (!mailbox->buffers[0] || !mailbox->buffers[1]) {
        ESP_LOGE(MAIN_TAG, "Failed to allocate mailbox buffers for %s", tag ? tag : "unknown");
        return false;
    }

    mailbox->activeIndex = 0;
    mailbox->frameLen = 0;
    mailbox->frameWidth = 0;
    mailbox->frameHeight = 0;
    mailbox->frameFormat = PIXFORMAT_GRAYSCALE;
    mailbox->frameSeq = 0;
    mailbox->frameCaptureUs = 0;
    mailbox->consumerTaskHandle = nullptr;
    return true;
}

static bool initPublishedHttpFrameStore()
{
    httpFrameBuffers[0] = (uint8_t*)heap_caps_malloc(kPublishedFrameMaxBytes, MALLOC_CAP_SPIRAM);
    httpFrameBuffers[1] = (uint8_t*)heap_caps_malloc(kPublishedFrameMaxBytes, MALLOC_CAP_SPIRAM);
    if (!httpFrameBuffers[0] || !httpFrameBuffers[1]) {
        ESP_LOGE(MAIN_TAG, "Failed to allocate published HTTP frame buffers");
        return false;
    }

    activeHttpFrameLen = 0;
    activeHttpFrameWidth = TF_IMAGE_INPUT_SIZE;
    activeHttpFrameHeight = TF_IMAGE_INPUT_SIZE;
    activeHttpFrameFormat = PIXFORMAT_GRAYSCALE;
    activeHttpFrameSeq = 0;
    activeHttpFramePublishUs = 0;
    return true;
}

static void publishToMailbox(FrameMailbox* mailbox,
                             const uint8_t* src,
                             size_t srcLen,
                             uint16_t width,
                             uint16_t height,
                             pixformat_t format,
                             int64_t captureUs)
{
    if (!mailbox || !src || srcLen == 0) {
        return;
    }

    size_t frameLen = srcLen;
    if (frameLen > kPublishedFrameMaxBytes) {
        ESP_LOGW(MAIN_TAG,
                 "Mailbox %s frame too large (%u > %u), truncating",
                 mailbox->tag ? mailbox->tag : "unknown",
                 (unsigned int)frameLen,
                 (unsigned int)kPublishedFrameMaxBytes);
        frameLen = kPublishedFrameMaxBytes;
    }

    uint8_t publishIndex = mailbox->activeIndex ^ 1;
    if (!mailbox->buffers[publishIndex]) {
        return;
    }

    memcpy(mailbox->buffers[publishIndex], src, frameLen);
    taskENTER_CRITICAL(&mailbox->lock);
    mailbox->frameLen = frameLen;
    mailbox->frameWidth = width;
    mailbox->frameHeight = height;
    mailbox->frameFormat = format;
    mailbox->activeIndex = publishIndex;
    mailbox->frameSeq++;
    mailbox->frameCaptureUs = captureUs;
    taskEXIT_CRITICAL(&mailbox->lock);

    if (mailbox->consumerTaskHandle) {
        xTaskNotifyGive(mailbox->consumerTaskHandle);
    }
}

static bool snapshotMailbox(FrameMailbox* mailbox, FrameSnapshot* snapshot)
{
    if (!mailbox || !snapshot) {
        return false;
    }

    taskENTER_CRITICAL(&mailbox->lock);
    const uint8_t activeIndex = mailbox->activeIndex;
    snapshot->len = mailbox->frameLen;
    snapshot->width = mailbox->frameWidth;
    snapshot->height = mailbox->frameHeight;
    snapshot->format = mailbox->frameFormat;
    snapshot->seq = mailbox->frameSeq;
    snapshot->captureUs = mailbox->frameCaptureUs;
    taskEXIT_CRITICAL(&mailbox->lock);

    snapshot->data = mailbox->buffers[activeIndex];
    return snapshot->data != nullptr && snapshot->len > 0;
}

static void publishHttpFrame(const uint8_t* src,
                             size_t srcLen,
                             uint16_t width,
                             uint16_t height,
                             pixformat_t format,
                             int64_t publishUs)
{
    if (!src || srcLen == 0) {
        return;
    }

    size_t frameLen = srcLen;
    if (frameLen > kPublishedFrameMaxBytes) {
        ESP_LOGW(HTTP_TAG,
                 "Published frame too large (%u > %u), truncating",
                 (unsigned int)frameLen,
                 (unsigned int)kPublishedFrameMaxBytes);
        frameLen = kPublishedFrameMaxBytes;
    }

    uint8_t publishIndex = activeHttpFrameIndex ^ 1;
    if (!httpFrameBuffers[publishIndex]) {
        return;
    }

    memcpy(httpFrameBuffers[publishIndex], src, frameLen);

    taskENTER_CRITICAL(&httpFrameMetaLock);
    activeHttpFrameLen = frameLen;
    activeHttpFrameWidth = width;
    activeHttpFrameHeight = height;
    activeHttpFrameFormat = format;
    activeHttpFrameIndex = publishIndex;
    activeHttpFrameSeq++;
    activeHttpFramePublishUs = publishUs;
    taskEXIT_CRITICAL(&httpFrameMetaLock);

    #if STOP_ACQUISITION_AFTER_1_FRAME
        if (activeHttpFrameSeq == 1) {
            pauseCameraAcquisition = true;
            ESP_LOGW(CAPTURE_TAG, "STOP_ACQUISITION_AFTER_1_FRAME active: acquisition paused after first frame");
        }
    #endif
}

static bool buildGray96Frame(const FrameSnapshot& snapshot,
                             uint8_t* grayscaleWorkspace,
                             size_t grayscaleWorkspaceLen,
                             uint8_t* gray96Buffer)
{
    if (!snapshot.data || !grayscaleWorkspace || !gray96Buffer) {
        return false;
    }

    if (snapshot.width < TF_IMAGE_INPUT_SIZE || snapshot.height < TF_IMAGE_INPUT_SIZE) {
        ESP_LOGW(CAPTURE_TAG, "Frame too small to resize to 96x96");
        return false;
    }

    const size_t pixelCount = (size_t)snapshot.width * (size_t)snapshot.height;
    if (pixelCount > grayscaleWorkspaceLen) {
        ESP_LOGW(CAPTURE_TAG,
                 "Frame scratch buffer too small (%u > %u)",
                 (unsigned int)pixelCount,
                 (unsigned int)grayscaleWorkspaceLen);
        return false;
    }

    switch (snapshot.format) {
        case PIXFORMAT_GRAYSCALE:
            return cropCenter(snapshot.data,
                              snapshot.width,
                              snapshot.height,
                              gray96Buffer,
                              TF_IMAGE_INPUT_SIZE,
                              TF_IMAGE_INPUT_SIZE,
                              1);

        case PIXFORMAT_RGB565:
            convertRgb565ToGrayscale((const uint16_t*)snapshot.data,
                                     grayscaleWorkspace,
                                     snapshot.width,
                                     snapshot.height);
            return cropCenter(grayscaleWorkspace,
                              snapshot.width,
                              snapshot.height,
                              gray96Buffer,
                              TF_IMAGE_INPUT_SIZE,
                              TF_IMAGE_INPUT_SIZE,
                              1);

        case PIXFORMAT_RGB888:
            convertRgb888ToGrayscale(snapshot.data,
                                     grayscaleWorkspace,
                                     snapshot.width,
                                     snapshot.height);
            return cropCenter(grayscaleWorkspace,
                              snapshot.width,
                              snapshot.height,
                              gray96Buffer,
                              TF_IMAGE_INPUT_SIZE,
                              TF_IMAGE_INPUT_SIZE,
                              1);

        case PIXFORMAT_JPEG: {
            camera_fb_t jpegFrame = {};
            jpegFrame.buf = (uint8_t*)snapshot.data;
            jpegFrame.len = snapshot.len;
            jpegFrame.width = snapshot.width;
            jpegFrame.height = snapshot.height;
            jpegFrame.format = PIXFORMAT_JPEG;

            uint8_t* decodedRgb565 = allocatingDecodeCameraJpeg(&jpegFrame,
                                                                 MALLOC_CAP_SPIRAM,
                                                                 JPEG_IMAGE_FORMAT_RGB565,
                                                                 JPEG_IMAGE_SCALE_0);
            if (!decodedRgb565) {
                ESP_LOGE(CAPTURE_TAG, "JPEG decode failed");
                return false;
            }

            convertRgb565ToGrayscale((const uint16_t*)decodedRgb565,
                                     grayscaleWorkspace,
                                     snapshot.width,
                                     snapshot.height);
            const bool ok = cropCenter(grayscaleWorkspace,
                                       snapshot.width,
                                       snapshot.height,
                                       gray96Buffer,
                                       TF_IMAGE_INPUT_SIZE,
                                       TF_IMAGE_INPUT_SIZE,
                                       1);
            heap_caps_free(decodedRgb565);
            return ok;
        }

        default:
            ESP_LOGW(CAPTURE_TAG, "Unsupported pixel format %d", snapshot.format);
            return false;
    }
}

// Background task: capture frames and hand them to downstream consumers.
void capture_task(void *arg)
{
    ESP_LOGI(CAPTURE_TAG, "Camera capture task started");
    uint32_t capturedFrames = 0;
    int64_t captureWindowStartUs = esp_timer_get_time();

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

        const int64_t captureUs = esp_timer_get_time();

        #if ENABLE_RGB_STREAM_TASK
            publishToMailbox(&streamMailbox,
                             frameBuffer->buf,
                             frameBuffer->len,
                             frameBuffer->width,
                             frameBuffer->height,
                             frameBuffer->format,
                             captureUs);
        #endif

        #if ENABLE_INFERENCE
            publishToMailbox(&inferenceMailbox,
                             frameBuffer->buf,
                             frameBuffer->len,
                             frameBuffer->width,
                             frameBuffer->height,
                             frameBuffer->format,
                             captureUs);
        #endif

        capturedFrames++;
        const int64_t elapsedUs = captureUs - captureWindowStartUs;
        if (elapsedUs >= 2000000) {
            const float captureFps = (capturedFrames * 1000000.0f) / (float)elapsedUs;
            ESP_LOGI(CAPTURE_TAG,
                     "Capture FPS: %.2f | latest frame=%dx%d fmt=%d len=%d",
                     captureFps,
                     frameBuffer->width,
                     frameBuffer->height,
                     frameBuffer->format,
                     frameBuffer->len);
            capturedFrames = 0;
            captureWindowStartUs = captureUs;
        }

        esp_camera_fb_return(frameBuffer);

        // Yield to networking/HTTP tasks to avoid burst-and-freeze behavior.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void stream_publish_task(void *arg)
{
    ESP_LOGI("HTTP_STREAM", "Stream publish task started");

    uint8_t* grayscaleWorkspace = (uint8_t*)heap_caps_malloc(kAcquiredFrameMaxPixels, MALLOC_CAP_8BIT);
    uint8_t* gray96Buffer = (uint8_t*)heap_caps_malloc(TF_IMAGE_INPUT_SIZE * TF_IMAGE_INPUT_SIZE, MALLOC_CAP_8BIT);
    if (!grayscaleWorkspace || !gray96Buffer) {
        ESP_LOGE("HTTP_STREAM", "Failed to allocate stream publish workspace");
        vTaskDelete(NULL);
        return;
    }

    uint32_t lastSeenSeq = 0;
    uint32_t publishedFrames = 0;
    int64_t publishWindowStartUs = esp_timer_get_time();

    while (true) {
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));

        FrameSnapshot snapshot = {};
        if (!snapshotMailbox(&streamMailbox, &snapshot) || snapshot.seq == lastSeenSeq) {
            continue;
        }

        const uint8_t* publishSrc = nullptr;
        size_t publishLen = 0;
        uint16_t publishWidth = snapshot.width;
        uint16_t publishHeight = snapshot.height;
        pixformat_t publishFormat = snapshot.format;

        if (snapshot.format == PIXFORMAT_JPEG) {
            publishSrc = snapshot.data;
            publishLen = snapshot.len;
        } else {
            #if STREAM_ORIGINALLY_ACQUIRED_IMAGE
                if (snapshot.format == PIXFORMAT_GRAYSCALE) {
                    publishSrc = snapshot.data;
                    publishLen = (size_t)snapshot.width * (size_t)snapshot.height;
                } else if (buildGray96Frame(snapshot,
                                            grayscaleWorkspace,
                                            kAcquiredFrameMaxPixels,
                                            gray96Buffer)) {
                    publishSrc = gray96Buffer;
                    publishLen = TF_IMAGE_INPUT_SIZE * TF_IMAGE_INPUT_SIZE;
                    publishWidth = TF_IMAGE_INPUT_SIZE;
                    publishHeight = TF_IMAGE_INPUT_SIZE;
                    publishFormat = PIXFORMAT_GRAYSCALE;
                }
            #else
                if (buildGray96Frame(snapshot,
                                     grayscaleWorkspace,
                                     kAcquiredFrameMaxPixels,
                                     gray96Buffer)) {
                    publishSrc = gray96Buffer;
                    publishLen = TF_IMAGE_INPUT_SIZE * TF_IMAGE_INPUT_SIZE;
                    publishWidth = TF_IMAGE_INPUT_SIZE;
                    publishHeight = TF_IMAGE_INPUT_SIZE;
                    publishFormat = PIXFORMAT_GRAYSCALE;
                }
            #endif
        }

        if (!publishSrc || publishLen == 0) {
            lastSeenSeq = snapshot.seq;
            continue;
        }

        const int64_t publishUs = esp_timer_get_time();
        publishHttpFrame(publishSrc,
                         publishLen,
                         publishWidth,
                         publishHeight,
                         publishFormat,
                         publishUs);

        lastSeenSeq = snapshot.seq;
        publishedFrames++;

        const int64_t elapsedUs = publishUs - publishWindowStartUs;
        if (elapsedUs >= 2000000) {
            const float publishFps = (publishedFrames * 1000000.0f) / (float)elapsedUs;
            ESP_LOGI("HTTP_STREAM",
                     "Publish FPS: %.2f | seq=%lu | frame=%dx%d fmt=%d len=%d",
                     publishFps,
                     (unsigned long)activeHttpFrameSeq,
                     publishWidth,
                     publishHeight,
                     publishFormat,
                     (unsigned int)publishLen);
            publishedFrames = 0;
            publishWindowStartUs = publishUs;
        }
    }
}



// HTTP handler — returns the latest captured frame payload
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
    uint16_t frameWidth = TF_IMAGE_INPUT_SIZE;
    uint16_t frameHeight = TF_IMAGE_INPUT_SIZE;
    pixformat_t frameFormat = PIXFORMAT_GRAYSCALE;
    uint32_t frameSeq = 0;
    int64_t framePublishUs = 0;
    taskENTER_CRITICAL(&httpFrameMetaLock);
    frameLen = activeHttpFrameLen;
    frameIndex = activeHttpFrameIndex;
    frameWidth = activeHttpFrameWidth;
    frameHeight = activeHttpFrameHeight;
    frameFormat = activeHttpFrameFormat;
    frameSeq = activeHttpFrameSeq;
    framePublishUs = activeHttpFramePublishUs;
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
    snprintf(lenBuf, sizeof(lenBuf), "%u", (unsigned int)frameLen);
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
    size_t sendLen = frameLen;
    if (frameLen > 0 && httpFrameBuffers[frameIndex]) {
        sendPtr = httpFrameBuffers[frameIndex];
    } else {
        // Send a blank frame until the first capture is published.
        static uint8_t blank[kPublishedFrameMaxBytes] = {0};
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

// HTTP handler — keeps one connection open and streams frames continuously.
esp_err_t streamTcpRgbCallback(httpd_req_t *req)
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
        size_t frameLen = 0;
        uint8_t frameIndex = 0;
        uint16_t frameWidth = TF_IMAGE_INPUT_SIZE;
        uint16_t frameHeight = TF_IMAGE_INPUT_SIZE;
        pixformat_t frameFormat = PIXFORMAT_GRAYSCALE;
        uint32_t frameSeq = 0;
        int64_t framePublishUs = 0;

        // Check if a new frame is ready
        taskENTER_CRITICAL(&httpFrameMetaLock);
        frameSeq = activeHttpFrameSeq;
        taskEXIT_CRITICAL(&httpFrameMetaLock);

        if (frameSeq == lastSeenSeq) {
            // No new frame yet; yield to give the capture task CPU time
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // Extract metadata safely
        taskENTER_CRITICAL(&httpFrameMetaLock);
        frameLen = activeHttpFrameLen;
        frameIndex = activeHttpFrameIndex;
        frameWidth = activeHttpFrameWidth;
        frameHeight = activeHttpFrameHeight;
        frameFormat = activeHttpFrameFormat;
        framePublishUs = activeHttpFramePublishUs;
        taskEXIT_CRITICAL(&httpFrameMetaLock);

        // Read directly from the active buffer (safe due to double-buffering architecture)
        const uint8_t *sendPtr = httpFrameBuffers[frameIndex];
        if (!sendPtr || frameLen == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        const int64_t nowUs = esp_timer_get_time();
        const int64_t ageUs = (framePublishUs > 0 && nowUs > framePublishUs) ? (nowUs - framePublishUs) : 0;

        // Format multipart headers for this individual frame, including telemetry
        int hlen = snprintf(part_buf, 256,
            "--" STREAM_BOUNDARY "\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %d\r\n"
            "X-Frame-Seq: %lu\r\n"
            "X-Frame-Age-Ms: %lld\r\n"
            "X-Frame-Width: %d\r\n"
            "X-Frame-Height: %d\r\n"
            "X-Frame-Format: %s\r\n"
            "\r\n",
            (frameFormat == PIXFORMAT_JPEG) ? "image/jpeg" : "application/octet-stream",
            (int)frameLen,
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

    uint8_t *frameSnapshot = (uint8_t *)heap_caps_malloc(kPublishedFrameMaxBytes, MALLOC_CAP_8BIT);
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

        if (frameLen > kPublishedFrameMaxBytes) {
            frameLen = kPublishedFrameMaxBytes;
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

    if (!initPublishedHttpFrameStore()) {
        return;
    }

    #if ENABLE_RGB_STREAM_TASK
        if (!initFrameMailbox(&streamMailbox, "stream")) {
            return;
        }
    #endif

    #if ENABLE_INFERENCE
        if (!initFrameMailbox(&inferenceMailbox, "inference")) {
            return;
        }
    #endif

    // HTTP server task (always on; transport-specific handlers are configured below).
    auto http_server_task = [](void* arg) {
        #if USE_TCP
            server.setCaptureHandler(captureRgbCallback);
            server.setStreamHandler(streamTcpRgbCallback);
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
            udp_stream_task,
            "udp_stream_task",
            6144,
            NULL,
            5,
            NULL,
            0
        );
    #endif

    #if ENABLE_RGB_STREAM_TASK
        xTaskCreatePinnedToCore(
            stream_publish_task,
            "stream_publish_task",
            6144,
            NULL,
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
            [](void* arg) { tf_wrapper.inference_task(arg); },
            "inference_task",
            6144,
            NULL,
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
        xTaskCreatePinnedToCore(capture_task,
                                "capture_task",
                                4096,
                                NULL,
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