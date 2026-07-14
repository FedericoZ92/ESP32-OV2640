
#include "wifi/wifi.h"
#include <cstring>
#include "esp_netif_ip_addr.h"
#include "esp_log.h"
#include "debug.h"

bool WifiManager::s_connected = false;
bool WifiManager::s_connectFailed = false;
bool WifiManager::s_stoppingSta = false;
uint8_t WifiManager::s_lastDisconnectReason = WIFI_REASON_UNSPECIFIED;
bool WifiManager::s_legacySecurityFallbackApplied = false;
int WifiManager::s_retryCount = 0;
int WifiManager::s_maxRetry = 5;

WifiManager::WifiManager() = default;
WifiManager::~WifiManager() = default;

esp_err_t WifiManager::init()
{
    static bool initialized = false;
    if (initialized) return ESP_OK;

    ESP_ERROR_CHECK(nvs_flash_init());
    ESP_ERROR_CHECK(esp_netif_init());
    esp_err_t err = esp_event_loop_create_default();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_ERROR_CHECK(err);
    }

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    initialized = true;
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

    return ESP_OK;
}

esp_err_t WifiManager::initSTA(const std::string &ssid, const std::string &password, int max_retry)
{
    static bool sta_created = false;
    if (!sta_created) {
        esp_netif_create_default_wifi_sta();
        sta_created = true;
    }

    // Register event handlers once
    static bool handlers_registered = false;
    if (!handlers_registered) {
        ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &WifiManager::eventHandler, nullptr));
        ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &WifiManager::eventHandler, nullptr));
        handlers_registered = true;
    }

    // Set mode & config
    wifi_config_t sta_config = {};
    strncpy((char *)sta_config.sta.ssid, ssid.c_str(), sizeof(sta_config.sta.ssid));
    strncpy((char *)sta_config.sta.password, password.c_str(), sizeof(sta_config.sta.password));
    // Improve compatibility with mixed WPA2/WPA3 home routers.
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    sta_config.sta.pmf_cfg.capable = true;
    sta_config.sta.pmf_cfg.required = false;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Prioritize low-latency HTTP polling over power saving.
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    s_connected = false;
    s_connectFailed = false;
    s_stoppingSta = false;
    s_lastDisconnectReason = WIFI_REASON_UNSPECIFIED;
    s_legacySecurityFallbackApplied = false;
    s_retryCount = 0;
    s_maxRetry = (max_retry > 0) ? max_retry : 1;
    ESP_LOGI(TAG, "Connecting to SSID '%s' (password len=%u, max retry=%d)",
             ssid.c_str(), (unsigned)password.length(), s_maxRetry);

    // Wait until connected or terminal failure is reported by event handler.
    while (!s_connected && !s_connectFailed) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    if (s_connected) {
        ESP_LOGI(TAG, "Connected to Wi-Fi");
        return ESP_OK;
    }

    s_stoppingSta = true;
    esp_wifi_stop();
    s_stoppingSta = false;
    ESP_LOGW(TAG, "Failed to connect after %d retries", s_retryCount);
    return ESP_FAIL;

}

void WifiManager::applyLegacySecurityFallback()
{
    if (s_legacySecurityFallbackApplied) {
        return;
    }

    wifi_config_t current_config = {};
    esp_err_t get_cfg_err = esp_wifi_get_config(WIFI_IF_STA, &current_config);
    if (get_cfg_err != ESP_OK) {
        ESP_LOGW(TAG, "Could not read STA config for legacy fallback: %s", esp_err_to_name(get_cfg_err));
        return;
    }

    current_config.sta.threshold.authmode = WIFI_AUTH_WPA_WPA2_PSK;
    current_config.sta.pmf_cfg.capable = false;
    current_config.sta.pmf_cfg.required = false;

    esp_err_t set_cfg_err = esp_wifi_set_config(WIFI_IF_STA, &current_config);
    if (set_cfg_err != ESP_OK) {
        ESP_LOGW(TAG, "Could not apply legacy STA security fallback: %s", esp_err_to_name(set_cfg_err));
        return;
    }

    s_legacySecurityFallbackApplied = true;
    ESP_LOGW(TAG,
             "Applied legacy STA security fallback (WPA/WPA2 + PMF disabled) after handshake timeout.");
}

const char *WifiManager::disconnectReasonToString(uint8_t reason)
{
    switch (reason) {
    case WIFI_REASON_UNSPECIFIED:
        return "UNSPECIFIED";
    case WIFI_REASON_AUTH_EXPIRE:
        return "AUTH_EXPIRE";
    case WIFI_REASON_AUTH_LEAVE:
        return "AUTH_LEAVE";
    case WIFI_REASON_ASSOC_EXPIRE:
        return "ASSOC_EXPIRE";
    case WIFI_REASON_ASSOC_TOOMANY:
        return "ASSOC_TOOMANY";
    case WIFI_REASON_NOT_AUTHED:
        return "NOT_AUTHED";
    case WIFI_REASON_NOT_ASSOCED:
        return "NOT_ASSOCED";
    case WIFI_REASON_ASSOC_LEAVE:
        return "ASSOC_LEAVE";
    case WIFI_REASON_ASSOC_NOT_AUTHED:
        return "ASSOC_NOT_AUTHED";
    case WIFI_REASON_DISASSOC_PWRCAP_BAD:
        return "DISASSOC_PWRCAP_BAD";
    case WIFI_REASON_DISASSOC_SUPCHAN_BAD:
        return "DISASSOC_SUPCHAN_BAD";
    case WIFI_REASON_IE_INVALID:
        return "IE_INVALID";
    case WIFI_REASON_MIC_FAILURE:
        return "MIC_FAILURE";
    case WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT:
        return "4WAY_HANDSHAKE_TIMEOUT";
    case WIFI_REASON_GROUP_KEY_UPDATE_TIMEOUT:
        return "GROUP_KEY_UPDATE_TIMEOUT";
    case WIFI_REASON_IE_IN_4WAY_DIFFERS:
        return "IE_IN_4WAY_DIFFERS";
    case WIFI_REASON_GROUP_CIPHER_INVALID:
        return "GROUP_CIPHER_INVALID";
    case WIFI_REASON_PAIRWISE_CIPHER_INVALID:
        return "PAIRWISE_CIPHER_INVALID";
    case WIFI_REASON_AKMP_INVALID:
        return "AKMP_INVALID";
    case WIFI_REASON_UNSUPP_RSN_IE_VERSION:
        return "UNSUPP_RSN_IE_VERSION";
    case WIFI_REASON_INVALID_RSN_IE_CAP:
        return "INVALID_RSN_IE_CAP";
    case WIFI_REASON_802_1X_AUTH_FAILED:
        return "802_1X_AUTH_FAILED";
    case WIFI_REASON_CIPHER_SUITE_REJECTED:
        return "CIPHER_SUITE_REJECTED";
    case WIFI_REASON_BEACON_TIMEOUT:
        return "BEACON_TIMEOUT";
    case WIFI_REASON_NO_AP_FOUND:
        return "NO_AP_FOUND";
    case WIFI_REASON_AUTH_FAIL:
        return "AUTH_FAIL";
    case WIFI_REASON_ASSOC_FAIL:
        return "ASSOC_FAIL";
    case WIFI_REASON_HANDSHAKE_TIMEOUT:
        return "HANDSHAKE_TIMEOUT";
    case WIFI_REASON_CONNECTION_FAIL:
        return "CONNECTION_FAIL";
    case WIFI_REASON_AP_TSF_RESET:
        return "AP_TSF_RESET";
    case WIFI_REASON_ROAMING:
        return "ROAMING";
    default:
        return "UNKNOWN";
    }

}

void WifiManager::eventHandler(void *arg, esp_event_base_t event_base,
                               int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        s_connected = false;
        s_connectFailed = false;
        s_retryCount = 0;
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        if (s_stoppingSta) {
            s_connected = false;
            return;
        }

        wifi_event_sta_disconnected_t *event = (wifi_event_sta_disconnected_t *)event_data;
        const uint8_t reason = event ? event->reason : WIFI_REASON_UNSPECIFIED;
        s_lastDisconnectReason = reason;

        if (s_retryCount < s_maxRetry) {
            if (reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT && !s_legacySecurityFallbackApplied) {
                applyLegacySecurityFallback();
            }
            esp_wifi_connect();
            s_retryCount++;
            ESP_LOGW(TAG,
                     "Disconnected: reason=%u (%s). Retrying Wi-Fi connection (%d/%d)...",
                     reason,
                     disconnectReasonToString(reason),
                     s_retryCount,
                     s_maxRetry);
            if (reason == WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT) {
                ESP_LOGW(TAG,
                         "Hint: 4-way handshake timeout usually means WPA credentials or AP security mismatch. "
                         "Recheck password, disable WPA3-only mode on router, and keep 2.4GHz WPA2 enabled.");
            }
        } else {
            if (!s_connectFailed) {
                ESP_LOGE(TAG,
                         "Disconnected: reason=%u (%s). Failed to connect to AP after %d retries",
                         reason,
                         disconnectReasonToString(reason),
                         s_retryCount);
            }
            s_connectFailed = true;
        }
        s_connected = false;
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "Got IP Address: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        s_connectFailed = false;
        s_lastDisconnectReason = WIFI_REASON_UNSPECIFIED;
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