#pragma once

#include "esp_camera.h"
#include "esp_err.h"
#include <cstdint>

class CameraDriver {
public:
    CameraDriver();
    ~CameraDriver();

    /**
     * @brief Initialize the OV2640 camera module.
     * @return esp_err_t - ESP_OK if successful, otherwise error code.
     */
    esp_err_t init();

    /**
     * @brief Capture a single frame from the camera.
     * @return camera_fb_t* - Pointer to the framebuffer containing the image.
     */
    camera_fb_t* captureFrame();

    /**
     * @brief Release a previously captured frame.
     * @param fb - The frame buffer to release.
     */
    void releaseFrame(camera_fb_t* fb);

private:
    camera_config_t config;

    void configureCamera();
};
