#include "http-server/http-server.h"
#include "define.h"
#include "debug.h"
#include "app-globals.h"
#include "http-server/http-frame-buffer.h"
#include "data-types/frame-mailbox.h"
#include "data-types/frame-snapshot.h"
#include <netinet/in.h>  // For ntohs, htons, sockaddr_in
#include <arpa/inet.h>   // For inet_ntoa, inet_aton
#include "image-editing/editing.h"
#include "http-server/udp-fram-header.h"
#include <sys/socket.h>

CameraHttpServer::CaptureCallback CameraHttpServer::s_captureCallback = nullptr;
CameraHttpServer::StreamCallback CameraHttpServer::s_streamCallback = nullptr;

CameraHttpServer::CameraHttpServer() = default;
CameraHttpServer::~CameraHttpServer() { stop(); }


#pragma region HTTP_CAPTURE_CALLBACK
// HTTP handler, returns the latest captured frame payload, uses httpd_resp_send (HTTP over TCP)
esp_err_t CameraHttpServer::captureRgbTcpCallback(httpd_req_t *req, HttpFrameBuffer* frameBuffer, portMUX_TYPE* frameMetaLock)
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
    uint16_t frameWidth = 0; //TF_IMAGE_INPUT_SIZE;
    uint16_t frameHeight = 0; // TF_IMAGE_INPUT_SIZE
    pixformat_t frameFormat; // = PIXFORMAT_GRAYSCALE; 
    uint32_t frameSeq = 0;
    int64_t framePublishUs = 0;

    ESP_LOGD(HTTP_TAG, "Capture request received, seq=%lu", (unsigned long)httpReqWindowLastSeq + 1);
    taskENTER_CRITICAL(frameMetaLock);
    frameLength = frameBuffer->getActiveHttpFrameLength();
    frameIndex = frameBuffer->getActiveHttpFrameIndex();
    frameWidth = frameBuffer->getActiveHttpFrameWidth();
    frameHeight = frameBuffer->getActiveHttpFrameHeight();
    frameFormat = frameBuffer->getActiveHttpFrameFormat();
    frameSeq = frameBuffer->getActiveHttpFrameSeq();
    framePublishUs = frameBuffer->getActiveHttpFramePublishUs();
    taskEXIT_CRITICAL(frameMetaLock);
    ESP_LOGD(HTTP_TAG, "Capture request metadata: seq=%lu, len=%u, %dx%d, format=%d, publishUs=%lld",
             (unsigned long)frameSeq,
             (unsigned int)frameLength,
             (unsigned int)frameWidth,
             (unsigned int)frameHeight,
             (int)frameFormat,
             (long long)framePublishUs);

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
    if (frameLength > 0 && frameBuffer->isBuffersInitialized(frameIndex)) {
        sendPtr = frameBuffer->getBuffer(frameIndex);
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

#pragma region STREAM_RGB_TCP_CALLBACK
// HTTP handler, keeps one connection open and streams frames continuously, uses httpd_resp_send_chunk (HTTP multipart over TCP)
esp_err_t CameraHttpServer::streamRgbTcpCallback(httpd_req_t *req, HttpFrameBuffer* frameBuffer, portMUX_TYPE* frameMetaLock)
{
    esp_err_t res = ESP_OK;
    char *part_buf = (char*)malloc(128);
    if (!part_buf) {
        ESP_LOGE("HTTP_STREAM", "Failed to allocate header buffer for streaming");
        return ESP_ERR_NO_MEM;
    }

    #define STREAM_BOUNDARY "123456789000000000000987654321"
    static const char* stream_boundary_str = "\r\n--" STREAM_BOUNDARY "\r\n";
    
    // 1. Prepare initial Multipart headers using framework methods
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

    uint32_t lastSeenSeq = 0;
    ESP_LOGI("HTTP_STREAM", "Started real-time multipart stream session");

    // 2. Stream loop
    while (true) {
        size_t frameLength = 0;
        uint8_t frameIndex = 0;
        pixformat_t frameFormat; 
        uint32_t frameSeq = 0;

        // Check if a new frame is ready
        taskENTER_CRITICAL(frameMetaLock);
        frameSeq = frameBuffer->getActiveHttpFrameSeq();
        taskEXIT_CRITICAL(frameMetaLock);

        if (frameSeq == lastSeenSeq) {
            vTaskDelay(1); // Crucial 10ms block keeps the task watchdog happy
            continue;
        }

        // Extract metadata safely from the active double buffer
        taskENTER_CRITICAL(frameMetaLock);
        frameLength = frameBuffer->getActiveHttpFrameLength();
        frameIndex = frameBuffer->getActiveHttpFrameIndex();
        frameFormat = frameBuffer->getActiveHttpFrameFormat();
        taskEXIT_CRITICAL(frameMetaLock);

        // Read directly from the active buffer
        const uint8_t *sendPtr = frameBuffer->getBuffer(frameIndex);
        if (!sendPtr || frameLength == 0) {
            vTaskDelay(1);
            continue;
        }

        // STEP A: Send the leading frame separator and boundary string
        res = httpd_resp_send_chunk(req, stream_boundary_str, strlen(stream_boundary_str));
        if (res != ESP_OK) break;

        // STEP B: Format clean content headers for this frame part
        int hlen = snprintf(part_buf, 128,
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "\r\n",
            (frameFormat == 4) ? "image/jpeg" : "application/octet-stream", // 4 maps to PIXFORMAT_JPEG
            frameLength
        );

        // STEP C: Send image content headers
        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res != ESP_OK) break;

        // STEP D: Send binary JPEG payload
        res = httpd_resp_send_chunk(req, (const char*)sendPtr, frameLength);
        if (res != ESP_OK) break;

        lastSeenSeq = frameSeq;
    } 

    ESP_LOGI("HTTP_STREAM", "Multipart stream session ended");
    free(part_buf);
    
    // Finalize the chunked stream cleanly for the framework
    httpd_resp_send_chunk(req, nullptr, 0);
    return res;
}

// HTTP handler, keeps one connection open and streams frames continuously, uses httpd_resp_send_chunk (HTTP multipart over TCP)
/*esp_err_t CameraHttpServer::streamRgbTcpCallback(httpd_req_t *req, HttpFrameBuffer* frameBuffer, portMUX_TYPE* frameMetaLock)
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

    //// 2. FORCE-FLUSH HEADERS IMMEDIATELY
    //// Sending a tiny, harmless 2-byte chunk (\r\n) triggers the HTTP server 
    //// to instantly send the HTTP "200 OK" and headers to the browser.
    //// This transitions the browser UI from "Connecting..." to "Streaming active...".
    //res = httpd_resp_send_chunk(req, "\r\n", 2);
    //if (res != ESP_OK) { free(part_buf); return res; }

    uint32_t lastSeenSeq = 0;
    ESP_LOGI("HTTP_STREAM", "Started real-time multipart stream session");

    // 3. Stream loop
    while (true) {
        size_t frameLength = 0;
        uint8_t frameIndex = 0;
        uint16_t frameWidth = 0; // TF_IMAGE_INPUT_SIZE
        uint16_t frameHeight = 0; // TF_IMAGE_INPUT_SIZE
        pixformat_t frameFormat; // = PIXFORMAT_GRAYSCALE;
        uint32_t frameSeq = 0;
        int64_t framePublishUs = 0;
        
        // Check if a new frame is ready
        ESP_LOGD(HTTP_TAG, "Waiting for new frame to stream, lastSeenSeq=%lu", (unsigned long)lastSeenSeq);
        taskENTER_CRITICAL(frameMetaLock);
        frameSeq = frameBuffer->getActiveHttpFrameSeq();
        taskEXIT_CRITICAL(frameMetaLock);

        if (frameSeq == lastSeenSeq) {
            // No new frame yet; yield to give the capture task CPU time
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        // Extract metadata safely
        taskENTER_CRITICAL(frameMetaLock);
        frameLength = frameBuffer->getActiveHttpFrameLength();
        frameIndex = frameBuffer->getActiveHttpFrameIndex();
        frameWidth = frameBuffer->getActiveHttpFrameWidth();
        frameHeight = frameBuffer->getActiveHttpFrameHeight();
        frameFormat = frameBuffer->getActiveHttpFrameFormat();
        framePublishUs = frameBuffer->getActiveHttpFramePublishUs();
        taskEXIT_CRITICAL(frameMetaLock);

        // Read directly from the active buffer (safe due to double-buffering architecture)
        const uint8_t *sendPtr = frameBuffer->getBuffer(frameIndex);
        if (!sendPtr || frameLength == 0) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        const int64_t nowUs = esp_timer_get_time();
        const int64_t ageUs = (framePublishUs > 0 && nowUs > framePublishUs) ? (nowUs - framePublishUs) : 0;

        // Format multipart headers for this individual frame, including telemetry
        //int hlen = snprintf(part_buf, 256,
        //    "--" STREAM_BOUNDARY "\r\n"
        //    "Content-Type: %s\r\n"
        //    "Content-Length: %zu\r\n"
        //   "X-Frame-Seq: %lu\r\n"
        //    "X-Frame-Age-Ms: %lld\r\n"
        //    "X-Frame-Width: %d\r\n"
        //    "X-Frame-Height: %d\r\n"
        //    "X-Frame-Format: %s\r\n"
        //    "\r\n",
        //    (frameFormat == PIXFORMAT_JPEG) ? "image/jpeg" : "application/octet-stream",
        //    (size_t)frameLength,
        //    (unsigned long)frameSeq,
        //    (long long)(ageUs / 1000),
        //    (int)frameWidth,
        //    (int)frameHeight,
        //    (frameFormat == PIXFORMAT_JPEG) ? "jpeg" : "gray8"
        //);
        // Format clean multipart headers for this individual frame
        int hlen = snprintf(part_buf, 256,
            "--" STREAM_BOUNDARY "\r\n"
            "Content-Type: %s\r\n"
            "Content-Length: %zu\r\n"
            "\r\n",
            (frameFormat == PIXFORMAT_JPEG) ? "image/jpeg" : "application/octet-stream",
            (size_t)frameLength
        );

        // Send boundary and custom headers
        res = httpd_resp_send_chunk(req, part_buf, hlen);
        if (res != ESP_OK) break;

        // Send binary frame buffer payload
        res = httpd_resp_send_chunk(req, (const char*)sendPtr, frameLength);
        if (res != ESP_OK) break;

        // Terminate part with a carriage return line feed
        res = httpd_resp_send_chunk(req, "\r\n", 2);
        if (res != ESP_OK) break;

        ESP_LOGD("HTTP_STREAM", "Streamed frame: seq=%lu, index=%u, len=%zu, %dx%d, format=%d, age_ms=%lld",
                 (unsigned long)frameSeq,
                 frameIndex,
                 frameLength,
                 (int)frameWidth,
                 (int)frameHeight,
                 (int)frameFormat,
                 (long long)(ageUs / 1000));

        lastSeenSeq = frameSeq;
    } // End of while loop

    ESP_LOGI("HTTP_STREAM", "Multipart stream session ended: %s", esp_err_to_name(res));
    free(part_buf);
    return res;
}*/

#pragma region INDEX_HTML
// HTML page for live view using <canvas>
#if USE_UDP
static const char *INDEX_HTML = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 Camera Live View</title>
</head>
<body>
  <h2>ESP32 Camera UDP Mode</h2>
  <p id="status">UDP streaming is active. Browser preview is disabled in UDP mode.</p>
  <p>Use a UDP receiver client on port 5001 and send a hello datagram first.</p>
  <p>Tip: keep this page open for device status logs and controls.</p>
  <label><input id="freezeToggle" type="checkbox"> Freeze capture (debug)</label>
  <script>
    const freezeToggle = document.getElementById('freezeToggle');
    let lastFreeze = null;

    async function syncFreeze() {
      const freezeValue = freezeToggle.checked ? '1' : '0';
      if (freezeValue === lastFreeze) {
        return;
      }
      lastFreeze = freezeValue;
      try {
        await fetch(`/capture.rgb?ts=${Date.now()}&freeze=${freezeValue}`, { cache: 'no-store' });
      } catch (_) {
        // Ignore: in UDP mode this endpoint may be disabled depending on server wiring.
      }
    }

    freezeToggle.addEventListener('change', syncFreeze);
  </script>
)rawliteral";
#elif USE_TCP_STREAMING
static const char *INDEX_HTML = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 Camera Live View</title>
  <style>
    body { font-family: sans-serif; margin: 20px; text-align: center; }
    img { display: block; margin: 20px auto; background: #000; max-width: 100%; height: auto; }
  </style>
</head>
<body>
  <h2>ESP32 Camera Live View (Streaming)</h2>
  <!-- Point the image src directly to the streaming endpoint -->
  <img src="/stream.rgb" alt="Live Stream">
</body>
</html>
)rawliteral";
#elif USE_TCP_POLLING
static const char *INDEX_HTML = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 Camera Live View</title>
  <style>
    body {
      font-family: sans-serif;
      margin: 20px;
    }
    #status {
      font-family: monospace;
      background: #f0f0f0;
      padding: 8px;
      border-radius: 4px;
      word-break: break-all;
    }
    canvas {
      display: block;
      margin-top: 10px;
      background: #000;
    }
  </style>
</head>
<body>
  <h2>ESP32 Camera Live View (Streaming)</h2>
  <p id="status">Connecting...</p>
  <label><input id="freezeToggle" type="checkbox"> Freeze capture (debug)</label>
  <canvas id="camCanvas" width="96" height="96"></canvas>

  <script>
    const canvas = document.getElementById('camCanvas');
    const ctx = canvas.getContext('2d');
    const statusEl = document.getElementById('status');
    const freezeToggle = document.getElementById('freezeToggle');

    let width = 0;
    let height = 0;
    let frameSize = 0;
    let imageData = null;
    let buf32 = null;

    let renderedFrames = 0;
    let errorCount = 0;
    let staleFrames = 0;
    let discardedFrames = 0;
    let lastSeq = -1;
    let lastAgeMs = 0;
    let lastDecodeMs = 0;
    let lastDrawMs = 0;
    let lastFrameLen = 0;
    let lastFrameFormat = 'gray8';
    let reqCount = 0;
    let lastFpsTime = performance.now();
    let fpsFrames = 0;

    function ensureCanvasSize(newWidth, newHeight) {
      if (newWidth > 0 && newHeight > 0 && (newWidth !== width || newHeight !== height)) {
        width = newWidth;
        height = newHeight;
        frameSize = width * height;
        canvas.width = width;
        canvas.height = height;
        imageData = ctx.createImageData(width, height);
        buf32 = new Uint32Array(imageData.data.buffer);
      }
    }

    ensureCanvasSize(96, 96);

    function drawGrayFrame(gray) {
      const len = frameSize;
      const data = buf32;
      for (let i = 0; i < len; i++) {
        const val = gray[i];
        data[i] = (255 << 24) | (val << 16) | (val << 8) | val;
      }
      ctx.putImageData(imageData, 0, 0);
      renderedFrames++;
      fpsFrames++;
      updateStats();
    }

    async function drawJpegFrame(buffer) {
      const decodeStart = performance.now();
      const blob = new Blob([buffer], { type: 'image/jpeg' });
      const bitmap = await createImageBitmap(blob);
      lastDecodeMs = performance.now() - decodeStart;

      ensureCanvasSize(bitmap.width, bitmap.height);
      const drawStart = performance.now();
      ctx.drawImage(bitmap, 0, 0, width, height);
      bitmap.close();
      renderedFrames++;
      fpsFrames++;
      lastDrawMs = performance.now() - drawStart;
      updateStats();
    }

    function updateStats() {
      const now = performance.now();
      const elapsed = now - lastFpsTime;
      if (elapsed >= 1000) {
        const fps = (fpsFrames * 1000 / elapsed).toFixed(1);
        statusEl.textContent = `Frames: ${renderedFrames} | StreamFPS: ${fps} | fmt: ${lastFrameFormat} | stale: ${staleFrames} | discard: ${discardedFrames} | age: ${lastAgeMs}ms | decode: ${lastDecodeMs.toFixed(1)}ms | draw: ${lastDrawMs.toFixed(1)}ms | len: ${lastFrameLen}`;
        fpsFrames = 0;
        lastFpsTime = now;
      }
    }

    async function pollFrames() {
      try {
        const freezeValue = freezeToggle.checked ? '1' : '0';
        const response = await fetch(`/capture.rgb?ts=${Date.now()}&freeze=${freezeValue}`, { cache: 'no-store' });
        if (!response.ok) {
          throw new Error(`HTTP ${response.status}`);
        }

        reqCount++;
        const seqHeader = response.headers.get('X-Frame-Seq');
        const ageHeader = response.headers.get('X-Frame-Age-Ms');
        const lenHeader = response.headers.get('X-Frame-Len');
        const formatHeader = response.headers.get('X-Frame-Format');
        const contentType = response.headers.get('Content-Type') || '';
        const widthHeader = response.headers.get('X-Frame-Width');
        const heightHeader = response.headers.get('X-Frame-Height');

        const seq = seqHeader ? Number(seqHeader) : -1;
        lastAgeMs = ageHeader ? Number(ageHeader) : 0;
        lastFrameLen = lenHeader ? Number(lenHeader) : 0;
        lastFrameFormat = formatHeader || (contentType.includes('image/jpeg') ? 'jpeg' : 'gray8');
        const fWidth = widthHeader ? Number(widthHeader) : width;
        const fHeight = heightHeader ? Number(heightHeader) : height;
        ensureCanvasSize(fWidth, fHeight);

        if (seq >= 0) {
          if (lastSeq >= 0 && seq === lastSeq) {
            staleFrames++;
          }
          lastSeq = seq;
        }

        const decodeStart = performance.now();
        const buffer = await response.arrayBuffer();
        const bytes = new Uint8Array(buffer);
        const looksLikeJpeg = bytes.length >= 2 && bytes[0] === 0xFF && bytes[1] === 0xD8;

        if (lastFrameFormat === 'jpeg' || looksLikeJpeg) {
          lastFrameFormat = 'jpeg';
          await drawJpegFrame(buffer);
        } else {
          lastDecodeMs = performance.now() - decodeStart;
          if (bytes.length >= frameSize) {
            const drawStart = performance.now();
            drawGrayFrame(bytes.subarray(0, frameSize));
            lastDrawMs = performance.now() - drawStart;
          } else {
            discardedFrames++;
          }
        }

        errorCount = 0;
      } catch (err) {
        errorCount++;
        statusEl.textContent = `Waiting for frames... (${errorCount}) ${err && err.message ? err.message : ''}`;
      } finally {
        setTimeout(pollFrames, 30);
      }
    }

    pollFrames();
  </script>
</body>
</html>
)rawliteral";
#endif

#pragma region HTTP_SERVER_METHODS
// start HTTP Server
esp_err_t CameraHttpServer::start(uint16_t port)
{
    if (serverHandle) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.core_id = CORE_ID_HTTP_SERVER;
    config.keep_alive_enable = true;
    config.max_open_sockets = 7; // Ensure enough sockets are open
    config.server_port = port;
    config.ctrl_port = port + 1000; // avoid conflict
    config.max_uri_handlers = 12;

    esp_err_t err = httpd_start(&serverHandle, &config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to start HTTP server: 0x%x", err);
        return err;
    }

    // --- Root handler: serves the HTML page ---
    httpd_uri_t uri_root = {
        .uri = "/",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            httpd_resp_set_type(req, "text/html");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            ESP_LOGI("CameraHttpServer", "Serving index page");
            return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
        },
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(serverHandle, &uri_root);

    // --- Legacy capture path: redirect to viewer page ---
    httpd_uri_t uri_capture_jpg_legacy = {
        .uri = "/capture.jpg",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
          httpd_resp_set_status(req, "302 Found");
          httpd_resp_set_hdr(req, "Location", "/");
          httpd_resp_set_hdr(req, "Cache-Control", "no-store");
          return httpd_resp_send(req, nullptr, 0);
        },
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(serverHandle, &uri_capture_jpg_legacy);

    // --- Capture RGB handler: serves raw grayscale frames ---
    httpd_uri_t uri_capture_rgb = {
        .uri = "/capture.rgb",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            if (CameraHttpServer::s_captureCallback) {
                httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
                httpd_resp_set_hdr(req, "Pragma", "no-cache");
                httpd_resp_set_hdr(req, "Expires", "0");
                return CameraHttpServer::s_captureCallback(req, &httpFrameBuffer, &httpFrameMetaLock);
            } else {
                httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No capture handler");
                return ESP_FAIL;
            }
        },
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(serverHandle, &uri_capture_rgb);

    // --- Streaming handler: serves continuous raw grayscale frames ---
    httpd_uri_t uri_stream_rgb = {
        .uri = "/stream.rgb",
        .method = HTTP_GET,
        .handler = [](httpd_req_t *req) -> esp_err_t {
            if (CameraHttpServer::s_streamCallback) {
              httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
              httpd_resp_set_hdr(req, "Pragma", "no-cache");
              httpd_resp_set_hdr(req, "Expires", "0");
                            return CameraHttpServer::s_streamCallback(req, &httpFrameBuffer, &httpFrameMetaLock);
            }

            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No stream handler");
            return ESP_FAIL;
        },
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(serverHandle, &uri_stream_rgb);

    ESP_LOGI(TAG, "HTTP server started on port %u", port);
    return ESP_OK;
}

// stop server
void CameraHttpServer::stop()
{
    if (serverHandle) {
        httpd_stop(serverHandle);
        serverHandle = nullptr;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

// set capture handler
void CameraHttpServer::setCaptureHandler(CaptureCallback callback)
{
    s_captureCallback = callback;
}

void CameraHttpServer::setStreamHandler(StreamCallback callback)
{
  s_streamCallback = callback;
}


// generic capture handler
esp_err_t CameraHttpServer::handleCapture(httpd_req_t *req, HttpFrameBuffer* frameBuffer, portMUX_TYPE* frameMetaLock)
{
    if (s_captureCallback) {
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        httpd_resp_set_hdr(req, "Pragma", "no-cache");
        httpd_resp_set_hdr(req, "Expires", "0");
        return s_captureCallback(req, frameBuffer, frameMetaLock);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No capture handler");
        return ESP_FAIL;
    }
}

/*esp_err_t CameraHttpServer::handleIndex(httpd_req_t *req, HttpFrameBuffer* frameBuffer, portMUX_TYPE* frameMetaLock)
{
    if(s_streamCallback) ---
      httpd_resp_set_type(req, "text/html");
      httpd_resp_set_hdr(req, "Cache-Control", "no-store");
      ESP_LOGI(TAG, "Serving index page");
      return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
}*/

#pragma region HTTP_STREAM_TASK
void CameraHttpServer::http_stream_publish_task(void *arg)
{
    AppTaskContext* ctx = static_cast<AppTaskContext*>(arg);
    if (!ctx || !ctx->httpFrameBuffer || !ctx->streamMailboxManager) {
      ESP_LOGE("HTTP_STREAM", "Invalid app task context for stream publish task");
      vTaskDelete(NULL);
      return;
    }

    HttpFrameBuffer* frameBuffer = ctx->httpFrameBuffer;
    FrameMailboxManager* streamManager = ctx->streamMailboxManager;

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
        bool snapshotResult = streamManager->snapshot(&snapshot);
        if (!snapshotResult || snapshot.seq == lastSeenSeq) {
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
                                            gray96Buffer,
                                            TF_IMAGE_INPUT_SIZE)) {
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
                                     gray96Buffer,
                                     TF_IMAGE_INPUT_SIZE)) {
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
        frameBuffer->publishHttpFrame(publishSrc,
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
                     (unsigned long)frameBuffer->getActiveHttpFrameSeq(),
                     publishWidth,
                     publishHeight,
                     publishFormat,
                     (unsigned int)publishLen);
            publishedFrames = 0;
            publishWindowStartUs = publishUs;
        }
    }
}

#pragma region UDP_STREAM_TASK
void CameraHttpServer::udp_stream_task(void *arg)
{
  AppTaskContext* ctx = static_cast<AppTaskContext*>(arg);
  if (!ctx || !ctx->httpFrameBuffer) {
    ESP_LOGE("UDP_STREAM", "Invalid app task context for UDP stream task");
    vTaskDelete(NULL);
    return;
  }

  HttpFrameBuffer* frameBuffer = ctx->httpFrameBuffer;
  portMUX_TYPE* frameMetaLock = ctx->httpFrameMetaLock;

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
        if (frameMetaLock) {
            taskENTER_CRITICAL(frameMetaLock);
            frameLen = frameBuffer->getActiveHttpFrameLength();
            frameIndex = frameBuffer->getActiveHttpFrameIndex();
            frameSeq = frameBuffer->getActiveHttpFrameSeq();
            framePublishUs = frameBuffer->getActiveHttpFramePublishUs();
            taskEXIT_CRITICAL(frameMetaLock);
        } else {
            frameLen = frameBuffer->getActiveHttpFrameLength();
            frameIndex = frameBuffer->getActiveHttpFrameIndex();
            frameSeq = frameBuffer->getActiveHttpFrameSeq();
            framePublishUs = frameBuffer->getActiveHttpFramePublishUs();
        }

        const uint8_t* activeBuffer = frameBuffer->getBuffer(frameIndex);
        if (frameLen == 0 || !activeBuffer) {
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

        memcpy(frameSnapshot, activeBuffer, frameLen);

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
