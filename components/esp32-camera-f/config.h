#pragma once

extern "C" {
    #include "esp_camera.h"
}
#include "esp_err.h"
#include <cstdint>

#ifdef CONFIG_IDF_TARGET_ESP32
#define BOARD_WROVER_KIT 1
#elif defined CONFIG_IDF_TARGET_ESP32S2
#define BOARD_CAMERA_MODEL_ESP32S2 1
#elif defined CONFIG_IDF_TARGET_ESP32S3
#define BOARD_CAMERA_MODEL_ESP32_S3_EYE 1
#endif

#define portTICK_RATE_MS              portTICK_PERIOD_MS

// WROVER-KIT PIN Map
#if BOARD_WROVER_KIT

#define PWDN_GPIO_NUM -1  //power down is not used
#define RESET_GPIO_NUM -1 //software reset will be performed
#define XCLK_GPIO_NUM 21
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27

#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 19
#define Y4_GPIO_NUM 18
#define Y3_GPIO_NUM 5
#define Y2_GPIO_NUM 4
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

// ESP32Cam (AiThinker) PIN Map
#elif BOARD_ESP32CAM_AITHINKER

#define PWDN_GPIO_NUM 32
#define RESET_GPIO_NUM -1 //software reset will be performed
#define XCLK_GPIO_NUM 0
#define SIOD_GPIO_NUM 26
#define SIOC_GPIO_NUM 27

#define Y9_GPIO_NUM 35
#define Y8_GPIO_NUM 34
#define Y7_GPIO_NUM 39
#define Y6_GPIO_NUM 36
#define Y5_GPIO_NUM 21
#define Y4_GPIO_NUM 19
#define Y3_GPIO_NUM 18
#define Y2_GPIO_NUM 5
#define VSYNC_GPIO_NUM 25
#define HREF_GPIO_NUM 23
#define PCLK_GPIO_NUM 22

#elif BOARD_CAMERA_MODEL_ESP32S2

#define PWDN_GPIO_NUM     -1
#define RESET_GPIO_NUM    -1

#define VSYNC_GPIO_NUM    21
#define HREF_GPIO_NUM     38
#define PCLK_GPIO_NUM     11
#define XCLK_GPIO_NUM     40

#define SIOD_GPIO_NUM     17
#define SIOC_GPIO_NUM     18

#define Y9_GPIO_NUM       39
#define Y8_GPIO_NUM       41
#define Y7_GPIO_NUM       42
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       3
#define Y4_GPIO_NUM       14
#define Y3_GPIO_NUM       37
#define Y2_GPIO_NUM       13

#elif BOARD_CAMERA_MODEL_ESP32_S3_EYE

#define PWDN_GPIO_NUM     43
#define RESET_GPIO_NUM    44

#define VSYNC_GPIO_NUM    6
#define HREF_GPIO_NUM     7
#define PCLK_GPIO_NUM     13
#define XCLK_GPIO_NUM     15

#define SIOD_GPIO_NUM     4
#define SIOC_GPIO_NUM     5

#define Y9_GPIO_NUM       16
#define Y8_GPIO_NUM       17
#define Y7_GPIO_NUM       18
#define Y6_GPIO_NUM       12
#define Y5_GPIO_NUM       11
#define Y4_GPIO_NUM       10
#define Y3_GPIO_NUM       9
#define Y2_GPIO_NUM       8

#endif

#define I2C_MASTER_SCL_IO           4      /*!< GPIO number used for I2C master clock */
#define I2C_MASTER_SDA_IO           5      /*!< GPIO number used for I2C master data  */
#define I2C_MASTER_NUM              0      /*!< I2C master i2c port number, the number of i2c peripheral interfaces available will depend on the chip */
#define I2C_MASTER_FREQ_HZ          100000 /*!< I2C master clock frequency */