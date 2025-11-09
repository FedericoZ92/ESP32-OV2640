idf_component_register(
    SRCS "wifi.cpp" 
        "http-server.cpp" 
        "camera_driver.cpp" 
        "tf-lite.cpp" 
        "main.cpp"
        "tflite-person-detect/person_detect_model_data.cc"
        "image-editing/tf-resize.cpp"
    PRIV_REQUIRES spi_flash
    REQUIRES esp32-camera-f esp_http_server esp_wifi esp_netif nvs_flash esp-tflite-micro
    INCLUDE_DIRS "." 
                "../components/esp32-camera-f"
                "../managed_components/espressif__esp-tflite-micro"
)
