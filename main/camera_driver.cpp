
#include "camera_driver.h"
#include "esp_log.h"

static const char *TAG = "CameraDriver";

// OV2640 pin map for ESP32-S3-CAM-N16R8
// https://www.oceanlabz.in/getting-started-with-esp32-s3-wroom-n16r8-cam-dev-board/?srsltid=AfmBOors-1xeo_-CM5mcneEFHgQY9ps0qX2SHt8gf-S7Ndizot0T4vzk
// in docs: schematics file from: https://www.homotix.it/vendita/moduli-wi-fi/scheda-esp32-s3-n16r8
// https://github.com/microrobotics/ESP32-S3-N16R8/blob/main/ESP32-S3-N16R8_User_Guide.pdf
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
    config.pixel_format = PIXFORMAT_GRAYSCALE; //PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_QQVGA; //FRAMESIZE_QVGA;
    config.jpeg_quality = 15;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM; //Store in internal RAM
    config.grab_mode = CAMERA_GRAB_LATEST;
}

esp_err_t CameraDriver::init() {
    esp_err_t err = esp_camera_init(&config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Camera initialization failed: 0x%x", err);
    } else {
        ESP_LOGI(TAG, "Camera successfully initialized");
    }
    return err;
}

camera_fb_t* CameraDriver::captureFrame() {
    camera_fb_t *fb = esp_camera_fb_get();
    if (!fb) {
        ESP_LOGE(TAG, "Failed to capture frame");
        return nullptr;
    }
    ESP_LOGI(TAG, "Captured frame size: %d bytes", fb->len);
    return fb;
}

void CameraDriver::releaseFrame(camera_fb_t* fb) {
    if (fb) {
        esp_camera_fb_return(fb);
        ESP_LOGI(TAG, "Frame buffer released");
    }
}


