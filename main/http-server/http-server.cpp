#include "http-server/http-server.h"
#include "define.h"
#include "app-globals.h"
#include "data-types/frame-mailbox.h"
#include "data-types/frame-snapshot.h"
#include <netinet/in.h>  // For ntohs, htons, sockaddr_in
#include <arpa/inet.h>   // For inet_ntoa, inet_aton
#include "image-editing/editing.h"
#include "http-server/udp-fram-header.h"

CameraHttpServer::CaptureCallback CameraHttpServer::s_captureCallback = nullptr;
CameraHttpServer::StreamCallback CameraHttpServer::s_streamCallback = nullptr;

CameraHttpServer::CameraHttpServer() = default;
CameraHttpServer::~CameraHttpServer() { stop(); }

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
#else
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
                return CameraHttpServer::s_captureCallback(req);
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
          return CameraHttpServer::s_streamCallback(req);
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
esp_err_t CameraHttpServer::handleCapture(httpd_req_t *req)
{
    if (s_captureCallback) {
        httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate, max-age=0");
        httpd_resp_set_hdr(req, "Pragma", "no-cache");
        httpd_resp_set_hdr(req, "Expires", "0");
        return s_captureCallback(req);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No capture handler");
        return ESP_FAIL;
    }
}

#pragma region HTTP_STREAM_TASK
void CameraHttpServer::http_stream_publish_task(void *arg)
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
        bool snapshotResult = streamMailboxManager.snapshot(&snapshot);
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
                                     kAcquiredFrameMaxPixels,
                                     gray96Buffer,
                                     TF_IMAGE_INPUT_SIZE)) {
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
        httpFrameBuffer.publishHttpFrame(publishSrc,
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
                     (unsigned long)httpFrameBuffer.getActiveHttpFrameSeq(),
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