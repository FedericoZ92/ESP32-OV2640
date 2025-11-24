#pragma once

#include "esp_log.h"
#include <string>


#define OV2640_TAG "ov2640"
#define CAPTURE_TAG "CaptureTask"
#define TF_TAG "TfLiteWrapper"
#define RAM_TAG "RAM"
#define LED_TAG "Led"
#define MAIN_TAG "Main"

void log_RAM_status(const std::string& header);

