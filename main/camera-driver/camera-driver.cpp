
#include "camera-driver.h"
#include "esp_log.h"
#include "define.h"
#include "debug.h"
#include "app-globals.h"
#include "data-types/frame-mailbox.h"
#include <esp_timer.h>

#define PWDN_GPIO_NUM     -1   // Power down not used
#define RESET_GPIO_NUM    -1   // Reset not used
#define XCLK_GPIO_NUM     15   // External clock to camera
#define SIOD_GPIO_NUM      4   // SDA
#define SIOC_GPIO_NUM      5   // SCL
#define Y9_GPIO_NUM       16   // D7
#define Y8_GPIO_NUM       17   // D6
#define Y7_GPIO_NUM       18   // D5
#define Y6_GPIO_NUM       12   // D4
#define Y5_GPIO_NUM       10   // D3
#define Y4_GPIO_NUM        8   // D2
#define Y3_GPIO_NUM        9   // D1
#define Y2_GPIO_NUM       11   // D0
#define VSYNC_GPIO_NUM     6
#define HREF_GPIO_NUM      7
#define PCLK_GPIO_NUM     13
// Sync signals

CameraDriver::CameraDriver() {
    configureCamera();
}

CameraDriver::~CameraDriver() {
    esp_camera_deinit();
}

void CameraDriver::configureCamera() {
    config.ledc_channel = LEDC_CHANNEL_0;
    config.ledc_timer = LEDC_TIMER_0;
    config.pin_d0 = Y2_GPIO_NUM;
    config.pin_d1 = Y3_GPIO_NUM;
    config.pin_d2 = Y4_GPIO_NUM;
    config.pin_d3 = Y5_GPIO_NUM;
    config.pin_d4 = Y6_GPIO_NUM;
    config.pin_d5 = Y7_GPIO_NUM;
    config.pin_d6 = Y8_GPIO_NUM;
    config.pin_d7 = Y9_GPIO_NUM;
    config.pin_xclk = XCLK_GPIO_NUM;
    config.pin_pclk = PCLK_GPIO_NUM;
    config.pin_vsync = VSYNC_GPIO_NUM;
    config.pin_href = HREF_GPIO_NUM;
    config.pin_sccb_sda = SIOD_GPIO_NUM;
    config.pin_sccb_scl = SIOC_GPIO_NUM;
    config.pin_pwdn = PWDN_GPIO_NUM;
    config.pin_reset = RESET_GPIO_NUM;
    config.xclk_freq_hz = 10000000; //was 20000000
    config.pixel_format = PIXFORMAT_JPEG; //PIXFORMAT_JPEG; //PIXFORMAT_GRAYSCALE; 
    //IMAGE_FRAME_SIZE_FOR_INFERENCE; // FRAMESIZE_QQVGA; //used with inference
    config.frame_size = FRAMESIZE_QQVGA;
    config.jpeg_quality = 12;
    config.fb_count = 3;
    config.fb_location = CAMERA_FB_IN_PSRAM; //Store in internal RAM
    config.grab_mode = CAMERA_GRAB_LATEST;
}

esp_err_t CameraDriver::init() 
{
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(CAMERA_TAG, "Camera initialization failed: 0x%x", err);
    } else {
        ESP_LOGI(CAMERA_TAG, "Camera successfully initialized");
    }

    // Flip image 180°
    sensor_t *s = esp_camera_sensor_get();
    if (s) {
        s->set_vflip(s, 1);
        s->set_hmirror(s, 1);
        ESP_LOGI(OV2640_TAG, "Camera image flipped 180°");
    } else {
        ESP_LOGW(OV2640_TAG, "Failed to get camera sensor handle for flipping");
    }

    return err;
}

camera_fb_t* CameraDriver::captureFrame() 
{
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(CAMERA_TAG, "Failed to capture frame");
        return nullptr;
    }
    ESP_LOGI(CAMERA_TAG, "Captured frame size: %d bytes", fb->len);
    return fb;
}

void CameraDriver::releaseFrame(camera_fb_t* fb) 
{
    if (fb) {
        esp_camera_fb_return(fb);
        ESP_LOGI(CAMERA_TAG, "Frame buffer released");
    }
}

// Background task: capture frames and hand them to downstream consumers.
void CameraDriver::capture_task(void *arg)
{
    ESP_LOGI(CAPTURE_TAG, "Camera capture task started");
    uint32_t capturedFrames = 0;
    int64_t captureWindowStartUs = esp_timer_get_time();

    while (true) {
        if (pauseCameraAcquisition) {
            // Freeze mode keeps serving the last published frame to isolate HTTP/network speed.
            vTaskDelay(pdMS_TO_TICKS(10));
            continue;
        }

        // --- Handle JPEG decoding or raw frame ---
        ESP_LOGD(CAPTURE_TAG, "Handle JPEG decoding or raw frame, mark checkpoint"); 
        cameraAcquisitionTimer.checkpoint();
        camera_fb_t *frameBuffer = esp_camera_fb_get();
        if (!frameBuffer) {
            ESP_LOGW(CAPTURE_TAG, "Failed to get frame buffer");
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        ESP_LOGD(CAPTURE_TAG, "Frame: %dx%d, len=%d, format=%d", frameBuffer->width, frameBuffer->height, frameBuffer->len, frameBuffer->format);
        cameraAcquisitionTimer.logCheckpoint(CAPTURE_TAG, "frame captured");

        const int64_t captureUs = esp_timer_get_time();

        #if ENABLE_RGB_STREAM_TASK
            streamMailboxManager.publish(
                                frameBuffer->buf,
                                frameBuffer->len,
                                frameBuffer->width,
                                frameBuffer->height,
                                frameBuffer->format,
                                captureUs);
        #endif
        #if ENABLE_INFERENCE
            inferenceMailboxManager.publish(
                                frameBuffer->buf,
                                frameBuffer->len,
                                frameBuffer->width,
                                frameBuffer->height,
                                frameBuffer->format,
                                captureUs);
        #endif

        capturedFrames++;
        const int64_t elapsedUs = captureUs - captureWindowStartUs;
        if (elapsedUs >= 2000000) {
            const float captureFps = (capturedFrames * 1000000.0f) / (float)elapsedUs;
            ESP_LOGI(CAPTURE_TAG,
                     "Capture FPS: %.2f | latest frame=%dx%d fmt=%d len=%d",
                     captureFps,
                     frameBuffer->width,
                     frameBuffer->height,
                     frameBuffer->format,
                     frameBuffer->len);
            capturedFrames = 0;
            captureWindowStartUs = captureUs;
        }

        esp_camera_fb_return(frameBuffer);

        // Yield to networking/HTTP tasks to avoid burst-and-freeze behavior.
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void CameraDriver::operateCameraResetSequence()
{
    gpio_reset_pin((gpio_num_t)PWDN_GPIO_NUM);
    gpio_reset_pin((gpio_num_t)RESET_GPIO_NUM);
    gpio_set_direction((gpio_num_t)PWDN_GPIO_NUM, GPIO_MODE_OUTPUT);
    gpio_set_direction((gpio_num_t)RESET_GPIO_NUM, GPIO_MODE_OUTPUT);

    gpio_set_level((gpio_num_t)PWDN_GPIO_NUM, 0);  // Power on
    gpio_set_level((gpio_num_t)RESET_GPIO_NUM, 0); // Hold reset
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level((gpio_num_t)RESET_GPIO_NUM, 1); // Release reset

}


