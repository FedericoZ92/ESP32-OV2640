#pragma once

#include "esp_http_server.h"
#include "esp_log.h"
#include <functional>
#include <string>

class CameraHttpServer {
public:
    using CaptureCallback = std::function<esp_err_t(httpd_req_t *req)>;

    CameraHttpServer();
    ~CameraHttpServer();

    esp_err_t start(uint16_t port = 80);
    void stop();

    void setCaptureHandler(CaptureCallback callback);

private:
    static esp_err_t handleCapture(httpd_req_t *req);
    static esp_err_t handleIndex(httpd_req_t *req);

    static inline const char *TAG = "CameraHttpServer";

    static CaptureCallback s_captureCallback;
    httpd_handle_t serverHandle = nullptr;
};
