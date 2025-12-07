#include "http-server/http-server.h"

CameraHttpServer::CaptureCallback CameraHttpServer::s_captureCallback = nullptr;

CameraHttpServer::CameraHttpServer() = default;
CameraHttpServer::~CameraHttpServer() { stop(); }

// ============================
// HTML page for live view
// ============================
static const char *INDEX_HTML = R"rawliteral(
<!DOCTYPE html>
<html>
<head>
  <meta charset="UTF-8">
  <meta name="viewport" content="width=device-width, initial-scale=1.0">
  <title>ESP32 Camera Live View</title>
  <style>
    body { background:#111; color:#eee; text-align:center; font-family:sans-serif; margin:0; }
    img  { border:2px solid #444; border-radius:8px; margin-top:10px; width:320px; height:auto; }
  </style>
</head>
<body>
  <h2>ESP32 Camera Live View</h2>
  <img id="cam" src="/capture.jpg" />
  <script>
    document.addEventListener('DOMContentLoaded', () => {
      const img = document.getElementById('cam');

      function reload() {
        img.src = '/capture.jpg?_=' + Date.now();
      }

      // When an image finishes loading, request the next one
      img.onload = () => setTimeout(reload, 10000);  // 10000 ms pause between frames
      img.onerror = () => setTimeout(reload, 10000); // retry slower on errors

      reload(); // start the loop
    });
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
        .handler = [](httpd_req_t *req) {
            httpd_resp_set_type(req, "text/html");
            httpd_resp_set_hdr(req, "Cache-Control", "no-store");
            ESP_LOGI("CameraHttpServer", "Serving index page");
            return httpd_resp_send(req, INDEX_HTML, HTTPD_RESP_USE_STRLEN);
        },
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(serverHandle, &uri_root);

    // --- Capture handler: serves JPEG frames ---
    httpd_uri_t uri_capture = {
        .uri = "/capture.jpg",
        .method = HTTP_GET,
        .handler = handleCapture,
        .user_ctx = nullptr
    };
    httpd_register_uri_handler(serverHandle, &uri_capture);

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
// /capture.jpg handler
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
