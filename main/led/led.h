#pragma once

#include "driver/rmt.h"
#include "esp_log.h"

class LedController {
public:
    LedController(gpio_num_t gpio = GPIO_NUM_48);
    void turnRedLedOn();
    void turnGreenLedOn();
    void turnBlueLedOn();
    void turnLedOff();

private:
    void sendColor(uint8_t red, uint8_t green, uint8_t blue);
    gpio_num_t led_gpio;
};