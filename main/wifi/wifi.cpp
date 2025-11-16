
#include "wifi/wifi.h"
#include <cstring>
#include "esp_netif_ip_addr.h"
#include "esp_log.h"
#include "debug.h"

bool WifiManager::s_connected = false;
int WifiManager::s_retryCount = 0;

WifiManager::WifiManager() = default;
WifiManager::~WifiManager() = default;

esp_err_t WifiManager::init()
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    return ESP_OK;
}

esp_err_t WifiManager::initAP(const std::string &ssid,
                              const std::string &password,
                              uint8_t channel,
                              uint8_t max_conn)
{
    esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    wifi_config_t ap_config = {};
    strncpy((char *)ap_config.ap.ssid, ssid.c_str(), sizeof(ap_config.ap.ssid));
    strncpy((char *)ap_config.ap.password, password.c_str(), sizeof(ap_config.ap.password));
    ap_config.ap.ssid_len = ssid.length();
    ap_config.ap.channel = channel;
    ap_config.ap.max_connection = max_conn;
    ap_config.ap.authmode = password.empty() ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(OV2640_TAG, "Wi-Fi AP started. SSID: %s, PASS: %s", ssid.c_str(),
             password.empty() ? "<open>" : password.c_str());
    ESP_LOGI(OV2640_TAG, "Access Point IP: 192.168.4.1");

    return ESP_OK;
}

esp_err_t WifiManager::initSTA(const std::string &ssid,
                               const std::string &password,
                               int max_retry)
{
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiManager::eventHandler, nullptr));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiManager::eventHandler, nullptr));

    wifi_config_t sta_config = {};
    strncpy((char *)sta_config.sta.ssid, ssid.c_str(), sizeof(sta_config.sta.ssid));
    strncpy((char *)sta_config.sta.password, password.c_str(), sizeof(sta_config.sta.password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(OV2640_TAG, "Connecting to SSID:%s ...", ssid.c_str());

    s_retryCount = 0;
    while (!s_connected && s_retryCount < max_retry) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (s_connected)
        ESP_LOGI(OV2640_TAG, "Connected to Wi-Fi network");
    else
        ESP_LOGW(OV2640_TAG, "Failed to connect after %d retries", s_retryCount);

    return ESP_OK;
}

void WifiManager::eventHandler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_retryCount < 5) {
            esp_wifi_connect();
            s_retryCount++;
            ESP_LOGW(TAG, "Retrying Wi-Fi connection (%d)...", s_retryCount);
        } else {
            ESP_LOGE(TAG, "Failed to connect to AP");
        }
        s_connected = false;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
    }
}

std::string WifiManager::getLocalIP() const
{
    esp_netif_ip_info_t ip_info;
    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    if (netif && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        char buf[32];
        snprintf(buf, sizeof(buf), IPSTR, IP2STR(&ip_info.ip));
        return std::string(buf);
    }
    return "0.0.0.0";
}