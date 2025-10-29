
#include "http-server.h"

CameraHttpServer::CaptureCallback CameraHttpServer::s_captureCallback = nullptr;

CameraHttpServer::CameraHttpServer() = default;
CameraHttpServer::~CameraHttpServer() { stop(); }

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

void CameraHttpServer::stop()
{
    if (serverHandle) {
        httpd_stop(serverHandle);
        serverHandle = nullptr;
        ESP_LOGI(TAG, "HTTP server stopped");
    }
}

void CameraHttpServer::setCaptureHandler(CaptureCallback callback)
{
    s_captureCallback = callback;
}

esp_err_t CameraHttpServer::handleCapture(httpd_req_t *req)
{
    if (s_captureCallback) {
        return s_captureCallback(req);
    } else {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "No capture handler");
        return ESP_FAIL;
    }
}
