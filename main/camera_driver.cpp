
#include "camera_driver.h"
#include "esp_log.h"

static const char *TAG = "CameraDriver";

// Define OV2640 Camera Pin Mapping (adjust for your board)
// Power / Reset
#define PWDN_GPIO_NUM    -1   // Power-down pin not used
#define RESET_GPIO_NUM   -1   // Reset pin not used
// Clock
#define XCLK_GPIO_NUM    45   // Camera XCLK input
#define PCLK_GPIO_NUM     5   // Pixel clock output from camera
// SCCB (I2C-like) interface
#define SIOD_GPIO_NUM     1   // SDA
#define SIOC_GPIO_NUM     2   // SCL
// Camera data pins
#define Y2_GPIO_NUM      39   // D0
#define Y3_GPIO_NUM      40   // D1
#define Y4_GPIO_NUM      41   // D2
#define Y5_GPIO_NUM       4   // D3
#define Y6_GPIO_NUM       7   // D4
#define Y7_GPIO_NUM       8   // D5
#define Y8_GPIO_NUM      46   // D6
#define Y9_GPIO_NUM      48   // D7

// Sync signals
#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM    42

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
    config.xclk_freq_hz = 20000000;
    config.pixel_format = PIXFORMAT_JPEG;
    config.frame_size = FRAMESIZE_QVGA;
    config.jpeg_quality = 10;
    config.fb_count = 1;
    config.fb_location = CAMERA_FB_IN_DRAM;  // Store in internal RAM
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
