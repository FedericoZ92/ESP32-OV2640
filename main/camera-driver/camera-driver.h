#pragma once

extern "C" {
    #include "esp_camera.h"
}
#include "esp_err.h"
#include "data-types/frame-mailbox.h"
#include <cstdint>

class CameraDriver {
public:
    CameraDriver();
    ~CameraDriver();

    // Initialize the OV2640 camera module. Return esp_err_t - ESP_OK if successful, otherwise error code.
    esp_err_t init(FrameMailboxManager* streamMailboxManagerPtr, FrameMailboxManager* inferenceMailboxManagerPtr);

    // Capture a single frame from the camera. Return camera_fb_t* - Pointer to the framebuffer containing the image.
    camera_fb_t* captureFrame();

    // Release a previously captured frame. The frame buffer to release.
    void releaseFrame(camera_fb_t* fb);

    // Background task: capture frames and hand them to downstream consumers.
    void capture_task(CameraDriver* cameraPtr);

    FrameMailboxManager* getMutableStreamMailboxManagerPtr();
    FrameMailboxManager* getMutableInferenceMailboxManagerPtr();

private:
    camera_config_t config;
    FrameMailboxManager* streamMailboxManagerPtr_ = nullptr;
    FrameMailboxManager* inferenceMailboxManagerPtr_ = nullptr;

    void configureCamera();
};
