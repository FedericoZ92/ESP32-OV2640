#include "http-server/http-server.h"

CameraHttpServer::CaptureCallback CameraHttpServer::s_captureCallback = nullptr;

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
  <canvas id="camCanvas" width="96" height="96"></canvas>
  <script>
    const canvas = document.getElementById('camCanvas');
    const ctx = canvas.getContext('2d');
    const width = 96;
    const height = 96;
    const imageData = ctx.createImageData(width, height);

    async function fetchFrame() {
      try {
        const response = await fetch('/capture.rgb');
        const buffer = await response.arrayBuffer();
        const gray = new Uint8Array(buffer);

        for (let i = 0; i < gray.length; i++) {
          const j = i * 4;
          imageData.data[j + 0] = gray[i]; // R
          imageData.data[j + 1] = gray[i]; // G
          imageData.data[j + 2] = gray[i]; // B
          imageData.data[j + 3] = 255;     // A
        }

        ctx.putImageData(imageData, 0, 0);
      } catch (err) {
        console.error(err);
      } finally {
        setTimeout(fetchFrame, 1000); // next frame in 1000ms
      }
    }

    fetchFrame();
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