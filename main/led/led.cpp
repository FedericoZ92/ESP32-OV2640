
#include "led/led.h"
//#include "../debug.h


LedController::LedController(gpio_num_t gpio) : led_gpio(gpio)
{
    rmt_config_t config;
    config.rmt_mode = RMT_MODE_TX;
    config.channel = RMT_CHANNEL_0;
    config.gpio_num = led_gpio;
    config.clk_div = 2;
    config.mem_block_num = 1;

    config.tx_config.loop_en = false;
    config.tx_config.carrier_en = false;
    config.tx_config.idle_output_en = true;
    config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;
    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(config.channel, 0, 0));
}

void LedController::sendColor(uint8_t red, uint8_t green, uint8_t blue)
{
    // WS2812 expects GRB order
    uint8_t grb[3] = { green, red, blue };

    // You would need a waveform generator here to convert GRB to RMT items
    // For simplicity, use a known library or prebuilt waveform if available
    //ESP_LOGI(LED_TAG, "Sending color: R=%d G=%d B=%d", red, green, blue);
    // Placeholder: actual waveform generation needed here
}

void LedController::turnRedLedOn()  { sendColor(255, 0, 0); }
void LedController::turnGreenLedOn(){ sendColor(0, 255, 0); }
void LedController::turnBlueLedOn() { sendColor(0, 0, 255); }
void LedController::turnLedOff()    { sendColor(0, 0, 0); }