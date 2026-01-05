#include "red-led.h"

RedLedController::RedLedController()
{
    configureLedGpio2();
}


void RedLedController::configureLedGpio2()
{
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT; 
    io_conf.pin_bit_mask = (1ULL << GPIO_NUM_2);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf); 
}

void RedLedController::setLedGpio2(bool value)
{
    gpio_set_level(GPIO_NUM_2, (uint32_t)value);   
}