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
</head>
<body>
  <h2>ESP32 Camera Live View (Grayscale)</h2>
  <p id="status">Connecting...</p>
  <label><input id="freezeToggle" type="checkbox"> Freeze capture (debug)</label>
  <canvas id="camCanvas" width="96" height="96"></canvas>
  <script>
    const canvas = document.getElementById('camCanvas');
    const ctx = canvas.getContext('2d');
    const statusEl = document.getElementById('status');
    const freezeToggle = document.getElementById('freezeToggle');
    const width = 96;
    const height = 96;
    const imageData = ctx.createImageData(width, height);
    const frameSize = width * height;
    let renderedFrames = 0;
    let errorCount = 0;
    let staleFrames = 0;
    let discardedFrames = 0;
    let lastSeq = -1;
    let lastAgeMs = 0;
    let lastFetchMs = 0;
    let lastDecodeMs = 0;
    let lastDrawMs = 0;
    let lastFrameLen = 0;
    let reqCount = 0;
    let lastFpsTime = performance.now();
    let fpsFrames = 0;
    const pollDelayMs = 30;

    function drawGrayFrame(gray) {
      for (let i = 0; i < frameSize; i++) {
        const j = i * 4;
        imageData.data[j + 0] = gray[i];
        imageData.data[j + 1] = gray[i];
        imageData.data[j + 2] = gray[i];
        imageData.data[j + 3] = 255;
      }
      ctx.putImageData(imageData, 0, 0);
      renderedFrames++;
      fpsFrames++;

      const now = performance.now();
      const elapsed = now - lastFpsTime;
      if (elapsed >= 1000) {
        const fps = (fpsFrames * 1000 / elapsed).toFixed(1);
        const reqFps = (reqCount * 1000 / elapsed).toFixed(1);
        statusEl.textContent = `Frames: ${renderedFrames} | drawFPS: ${fps} | reqFPS: ${reqFps} | stale: ${staleFrames} | discard: ${discardedFrames} | age: ${lastAgeMs}ms | net: ${lastFetchMs.toFixed(1)}ms | decode: ${lastDecodeMs.toFixed(1)}ms | draw: ${lastDrawMs.toFixed(1)}ms | len: ${lastFrameLen} | poll: ${pollDelayMs}ms`;
        fpsFrames = 0;
        reqCount = 0;
        lastFpsTime = now;
      }
    }

    async function pollFrames() {
      try {
        const reqStart = performance.now();
        const freezeValue = freezeToggle.checked ? '1' : '0';
        const response = await fetch(`/capture.rgb?ts=${Date.now()}&freeze=${freezeValue}`, { cache: 'no-store' });
        const respReady = performance.now();
        reqCount++;
        lastFetchMs = respReady - reqStart;
        if (!response.ok) {
          throw new Error(`HTTP ${response.status}`);
        }

        const seqHeader = response.headers.get('X-Frame-Seq');
        const ageHeader = response.headers.get('X-Frame-Age-Ms');
        const lenHeader = response.headers.get('X-Frame-Len');
        const seq = seqHeader ? Number(seqHeader) : -1;
        lastAgeMs = ageHeader ? Number(ageHeader) : 0;
        lastFrameLen = lenHeader ? Number(lenHeader) : 0;
        if (seq >= 0) {
          if (lastSeq >= 0 && seq === lastSeq) {
            staleFrames++;
          }
          lastSeq = seq;
        }

        const decodeStart = performance.now();
        const buffer = await response.arrayBuffer();
        lastDecodeMs = performance.now() - decodeStart;
        const gray = new Uint8Array(buffer);
        if (gray.length >= frameSize) {
          const drawStart = performance.now();
          drawGrayFrame(gray.subarray(0, frameSize));
          lastDrawMs = performance.now() - drawStart;
          errorCount = 0;
        } else {
          discardedFrames++;
          throw new Error(`short frame: ${gray.length}`);
        }
      } catch (err) {
        errorCount++;
        console.error(err);
        statusEl.textContent = `Waiting for frames... (${errorCount})`;
      } finally {
        setTimeout(pollFrames, pollDelayMs);
      }
    }

    statusEl.textContent = 'Connecting capture...';
    pollFrames();
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