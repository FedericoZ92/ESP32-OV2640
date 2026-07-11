#include "http-server/http-server.h"
#include "define.h"

CameraHttpServer::CaptureCallback CameraHttpServer::s_captureCallback = nullptr;
CameraHttpServer::StreamCallback CameraHttpServer::s_streamCallback = nullptr;

CameraHttpServer::CameraHttpServer() = default;
CameraHttpServer::~CameraHttpServer() { stop(); }

// ============================
// HTML page for live view using <canvas>
// ============================
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
  <canvas id="camCanvas" width="96" height="96"></canvas>
  <script>
    const canvas = document.getElementById('camCanvas');
    const ctx = canvas.getContext('2d');
    const statusEl = document.getElementById('status');
    const width = 96;
    const height = 96;
    const imageData = ctx.createImageData(width, height);
    const frameSize = width * height;
    const compileEnablePollingFallback = )rawliteral" ENABLE_POLLING_FALLBACK_JS R"rawliteral(;
    let renderedFrames = 0;
    let fallbackActive = false;

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
      statusEl.textContent = `Frames: ${renderedFrames}`;
    }

    function enablePollingFallback(reason) {
      if (!compileEnablePollingFallback) {
        console.warn('Polling fallback disabled at compile time:', reason);
        statusEl.textContent = 'Stream mode only (fallback disabled)';
        return;
      }

      if (fallbackActive) {
        return;
      }
      fallbackActive = true;
      console.warn('Switching to /capture.rgb polling:', reason);
      statusEl.textContent = 'Using polling fallback...';
      pollFrames();
    }

    async function pollFrames() {
      try {
        const response = await fetch(`/capture.rgb?ts=${Date.now()}`, { cache: 'no-store' });
        if (!response.ok) {
          throw new Error(`HTTP ${response.status}`);
        }

        const buffer = await response.arrayBuffer();
        const gray = new Uint8Array(buffer);
        if (gray.length >= frameSize) {
          drawGrayFrame(gray.subarray(0, frameSize));
        }
      } catch (err) {
        console.error(err);
        statusEl.textContent = `Polling error: ${err.message}`;
      } finally {
        setTimeout(pollFrames, 250);
      }
    }

    async function streamFrames() {
      try {
        statusEl.textContent = 'Connecting stream...';
        const response = await fetch('/stream.rgb', { cache: 'no-store' });
        if (!response.ok) {
          throw new Error(`HTTP ${response.status}`);
        }

        if (!response.body) {
          throw new Error('Readable stream not available');
        }

        const reader = response.body.getReader();
        let pending = new Uint8Array(0);

        while (true) {
          const { value, done } = await reader.read();
          if (done) {
            break;
          }

          const chunk = value || new Uint8Array(0);
          const merged = new Uint8Array(pending.length + chunk.length);
          merged.set(pending);
          merged.set(chunk, pending.length);
          pending = merged;

          while (pending.length >= frameSize) {
            const gray = pending.subarray(0, frameSize);
            drawGrayFrame(gray);
            pending = pending.subarray(frameSize);
          }
        }
      } catch (err) {
        console.error(err);
        enablePollingFallback(err.message || 'stream failed');
      } finally {
        if (!fallbackActive) {
          // Reconnect if stream closes.
          setTimeout(streamFrames, 200);
        }
      }
    }

    // If no frame arrives quickly, switch to polling mode.
    setTimeout(() => {
      if (compileEnablePollingFallback && renderedFrames === 0) {
        enablePollingFallback('no frames from stream');
      }
    }, 2000);

    streamFrames();
  </script>
</body>
</html>
)rawliteral";

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