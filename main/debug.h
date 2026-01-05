#pragma once

#include "esp_log.h"
#include <string>


#define OV2640_TAG  "_____ov2640" //11
#define CAPTURE_TAG "CaptureTask"
#define TF_TAG      "TfLiteWrapp"
#define RAM_TAG     "________RAM"
#define LED_TAG     "________Led"
#define MAIN_TAG    "_______Main"
#define JPEG_TAG    "_______Jpeg"
#define HTTP_TAG    "_______http"

void log_RAM_status(const std::string& header);

