idf_component_register(
    SRCS "wifi.cpp" "http-server.cpp" "camera_driver.cpp" "main.cpp"
    PRIV_REQUIRES spi_flash
    REQUIRES esp32-camera-f esp_http_server esp_wifi esp_netif nvs_flash
    INCLUDE_DIRS "." "../components/esp32-camera-f"
)
