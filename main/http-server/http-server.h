#pragma once

#include "esp_http_server.h"
#include "esp_log.h"
#include <functional>
#include <string>

class CameraHttpServer {
public:
    using CaptureCallback = std::function<esp_err_t(httpd_req_t *req)>;
    using StreamCallback = std::function<esp_err_t(httpd_req_t *req)>;

    CameraHttpServer();
    ~CameraHttpServer();

    esp_err_t start(uint16_t port = 80);
    void stop();

    void setCaptureHandler(CaptureCallback callback);
    void setStreamHandler(StreamCallback callback);

    void http_stream_publish_task(void *arg);
    void udp_stream_task(void *arg);

private:
    static esp_err_t handleCapture(httpd_req_t *req);
    static esp_err_t handleIndex(httpd_req_t *req);

    static inline const char *TAG = "CameraHttpServer";

    static CaptureCallback s_captureCallback;
    static StreamCallback s_streamCallback;
    httpd_handle_t serverHandle = nullptr;
};
