#pragma once

#include "driver/rmt.h"
#include "esp_log.h"
#include <vector>

#define LED_TAG "LED"

class LedController
{
public:
    explicit LedController(gpio_num_t gpio = GPIO_NUM_48, rmt_channel_t channel = RMT_CHANNEL_0);
    void turnRedLedOn();
    void turnGreenLedOn();
    void turnBlueLedOn();
    void turnLedOff();
    void sendColor(uint8_t red, uint8_t green, uint8_t blue);

    void configureLedGpio2();
    void setLedGpio2(bool value);

private:
    gpio_num_t led_gpio;
    rmt_channel_t rmt_channel;

    std::vector<rmt_item32_t> encodeWS2812(uint8_t red, uint8_t green, uint8_t blue);
};