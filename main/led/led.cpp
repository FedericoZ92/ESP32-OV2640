#include "led.h"

LedController::LedController(gpio_num_t gpio, rmt_channel_t channel)
    : led_gpio(gpio), rmt_channel(channel)
{
    // Configure RMT for WS2812
    rmt_config_t config = {};
    config.rmt_mode = RMT_MODE_TX;
    config.channel = rmt_channel;
    config.gpio_num = led_gpio;
    config.clk_div = 2; // 80MHz / 2 = 40MHz
    config.mem_block_num = 1;
    config.tx_config.loop_en = false;
    config.tx_config.carrier_en = false;
    config.tx_config.idle_output_en = true;
    config.tx_config.idle_level = RMT_IDLE_LEVEL_LOW;

    ESP_ERROR_CHECK(rmt_config(&config));
    ESP_ERROR_CHECK(rmt_driver_install(rmt_channel, 0, 0));

    ESP_LOGI(LED_TAG, "WS2812 LED configured on GPIO %d", led_gpio);
}

void LedController::turnRedLedOn()   { sendColor(255, 0, 0); }
void LedController::turnGreenLedOn() { sendColor(0, 255, 0); }
void LedController::turnBlueLedOn()  { sendColor(0, 0, 255); }
void LedController::turnLedOff()     { sendColor(0, 0, 0); }

void LedController::sendColor(uint8_t red, uint8_t green, uint8_t blue)
{
    auto items = encodeWS2812(red, green, blue);

    // Send via RMT
    ESP_ERROR_CHECK(rmt_write_items(rmt_channel, items.data(), items.size(), true));
    ESP_ERROR_CHECK(rmt_wait_tx_done(rmt_channel, pdMS_TO_TICKS(100)));
}

// Encode one pixel (GRB order) into RMT waveform
std::vector<rmt_item32_t> LedController::encodeWS2812(uint8_t red, uint8_t green, uint8_t blue)
{
    std::vector<rmt_item32_t> items;
    uint8_t colors[3] = { green, red, blue }; // WS2812 expects GRB

    for (int i = 0; i < 3; i++) {
        for (int bit = 7; bit >= 0; bit--) {
            bool is_one = colors[i] & (1 << bit);
            rmt_item32_t item;
            if (is_one) {
                // Logical 1: ~0.8us HIGH, ~0.45us LOW
                item.level0 = 1; item.duration0 = 32; // 32 * 12.5ns = 0.8us
                item.level1 = 0; item.duration1 = 18; // 18 * 12.5ns = 0.225us (~0.45?)
            } else {
                // Logical 0: ~0.4us HIGH, ~0.85us LOW
                item.level0 = 1; item.duration0 = 16; // 0.4us
                item.level1 = 0; item.duration1 = 34; // 0.85us
            }
            items.push_back(item);
        }
    }

    // Append reset
    rmt_item32_t reset = {};
    reset.level0 = 0; reset.duration0 = 500; // >50us
    reset.level1 = 0; reset.duration1 = 0;
    items.push_back(reset);

    return items;
}

void LedController::configureLedGpio2()
{
    gpio_config_t io_conf;
    io_conf.intr_type = GPIO_INTR_DISABLE;
    io_conf.mode = GPIO_MODE_OUTPUT; 
    io_conf.pin_bit_mask = (1ULL << GPIO_NUM_2);
    io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
    io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
    gpio_config(&io_conf); 
}

void LedController::setLedGpio2(bool value)
{
    gpio_set_level(GPIO_NUM_2, (uint32_t)value);   
}