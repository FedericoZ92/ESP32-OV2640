#pragma once

#include "esp_log.h"
#include <vector>
#include <driver/gpio.h>

#define LED_TAG "LED"

class RedLedController
{
public:
    explicit RedLedController();

    void configureLedGpio2();
    void setLedGpio2(bool value);


};