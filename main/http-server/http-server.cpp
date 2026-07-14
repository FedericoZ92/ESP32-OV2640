#include "http-server/http-server.h"
#include "define.h"

CameraHttpServer::CaptureCallback CameraHttpServer::s_captureCallback = nullptr;
CameraHttpServer::StreamCallback CameraHttpServer::s_streamCallback = nullptr;

CameraHttpServer::CameraHttpServer() = default;
CameraHttpServer::~CameraHttpServer() { stop(); }

// ============================
// HTML page for live view using <canvas>
// ============================
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

    // Helper function to search bytes in stream buffer
    function findSubarray(arr, sub) {
      const limit = arr.length - sub.length + 1;
      for (let i = 0; i < limit; i++) {
        let match = true;
        for (let j = 0; j < sub.length; j++) {
          if (arr[i+j] !== sub[j]) { match = false; break; }
        }
        if (match) return i;
      }
      return -1;
    }

    async function runStream() {
      try {
        const response = await fetch('/stream.rgb');
        if (!response.ok) throw new Error(`HTTP ${response.status}`);
        
        statusEl.textContent = "Streaming active...";
        const reader = response.body.getReader();
        const boundary = "--123456789000000000000987654321";
        const boundaryBytes = new TextEncoder().encode(boundary);
        
        let buffer = new Uint8Array(0);
        
        while (true) {
          if (freezeToggle.checked) {
            // If frozen for debugging, read but discard to avoid memory build-up
            const { value, done } = await reader.read();
            if (done) break;
            continue;
          }

          const { value, done } = await reader.read();
          if (done) break;
          
          // Append new packet data
          const temp = new Uint8Array(buffer.length + value.length);
          temp.set(buffer);
          temp.set(value, buffer.length);
          buffer = temp;
          
          while (true) {
            const firstBoundaryIdx = findSubarray(buffer, boundaryBytes);
            if (firstBoundaryIdx === -1) break;
            
            const secondBoundaryIdx = findSubarray(buffer.subarray(firstBoundaryIdx + boundaryBytes.length), boundaryBytes);
            if (secondBoundaryIdx === -1) break; // Need more data chunks
            
            const partStart = firstBoundaryIdx + boundaryBytes.length;
            const partEnd = partStart + secondBoundaryIdx;
            const part = buffer.subarray(partStart, partEnd);
            
            // Slice buffer & immediately instantiate new Uint8Array to force garbage collection of old segment
            buffer = new Uint8Array(buffer.subarray(partStart + secondBoundaryIdx));
            
            // Separate Headers and Body using double CRLF
            const doubleCRLFBytes = new Uint8Array([13, 10, 13, 10]);
            const headerEndIdx = findSubarray(part, doubleCRLFBytes);
            if (headerEndIdx === -1) continue;
            
            const headerStr = new TextDecoder().decode(part.subarray(0, headerEndIdx));
            const bodyBytes = part.subarray(headerEndIdx + 4);
            
            // Read HTTP headers of the part
            const headers = {};
            headerStr.split('\r\n').forEach(line => {
              const pts = line.split(':');
              if (pts.length >= 2) {
                headers[pts[0].trim().toLowerCase()] = pts.slice(1).join(':').trim();
              }
            });
            
            const seq = headers['x-frame-seq'] ? Number(headers['x-frame-seq']) : -1;
            lastAgeMs = headers['x-frame-age-ms'] ? Number(headers['x-frame-age-ms']) : 0;
            lastFrameLen = headers['x-frame-len'] ? Number(headers['x-frame-len']) : 0;
            lastFrameFormat = headers['x-frame-format'] || 'gray8';
            const fWidth = headers['x-frame-width'] ? Number(headers['x-frame-width']) : width;
            const fHeight = headers['x-frame-height'] ? Number(headers['x-frame-height']) : height;
            
            ensureCanvasSize(fWidth, fHeight);
            
            if (seq >= 0) {
              if (lastSeq >= 0 && seq === lastSeq) { staleFrames++; }
              lastSeq = seq;
            }
            
            const decodeStart = performance.now();
            if (lastFrameFormat === 'jpeg') {
              // Draw JPEG binary buffer
              await drawJpegFrame(bodyBytes.buffer.slice(bodyBytes.byteOffset, bodyBytes.byteOffset + lastFrameLen));
            } else {
              // Draw raw grayscale buffer
              lastDecodeMs = performance.now() - decodeStart;
              if (bodyBytes.length >= frameSize) {
                const drawStart = performance.now();
                drawGrayFrame(bodyBytes.subarray(0, lastFrameLen));
                lastDrawMs = performance.now() - drawStart;
                errorCount = 0;
              } else {
                discardedFrames++;
              }
            }
          }
        }
      } catch (err) {
        errorCount++;
        statusEl.textContent = `Stream lost, retrying... (${errorCount}) ${err.message || ''}`;
        console.error(err);
        setTimeout(runStream, 1000); // Wait 1 sec and reconnect
      }
    }

    runStream();
  </script>
</body>
</html>
)rawliteral";
#endif

// ============================
// Start HTTP Server
// ============================
esp_err_t CameraHttpServer::start(uint16_t port)
{
    if (serverHandle) {
        ESP_LOGW(TAG, "Server already running");
        return ESP_OK;
    }

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
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

// ============================
// Stop server
// ============================
void CameraHttpServer::stop()
{
    if (serverHandle) {
        httpd_stop(serverHandle);
        serverHandle = nullptr;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

// ============================
// Set capture handler
// ============================
void CameraHttpServer::setCaptureHandler(CaptureCallback callback)
{
    s_captureCallback = callback;
}

void CameraHttpServer::setStreamHandler(StreamCallback callback)
{
  s_streamCallback = callback;
}

// ============================
// Generic capture handler
// ============================
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