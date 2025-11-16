#pragma once

#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "nvs_flash.h"
#include <string>

class WifiManager {
public:
    WifiManager();
    ~WifiManager();

    esp_err_t init();  // init NVS + netif + event loop (must be called first)

    esp_err_t initAP(const std::string &ssid,
                     const std::string &password,
                     uint8_t channel = 1,
                     uint8_t max_conn = 4);

    esp_err_t initSTA(const std::string &ssid,
                      const std::string &password,
                      int max_retry = 5);

    std::string getLocalIP() const;

private:
    static inline const char *TAG = "WifiManager";
    static void eventHandler(void *arg, esp_event_base_t event_base,
                             int32_t event_id, void *event_data);

    static bool s_connected;
    static int s_retryCount;
};
