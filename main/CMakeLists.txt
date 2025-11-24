idf_component_register(
    SRCS "wifi/wifi.cpp"
         "http-server/http-server.cpp"
         "camera-driver/camera-driver.cpp"
         "tf-lite/tf-lite.cpp"
         "main.cpp"
         "tflite-person-detect/person_detect_model_data.cc"
         "image-editing/editing.cpp"
         "debug.cpp"
         "led/led.cpp"
         "psram/psram.cpp"

    INCLUDE_DIRS "."
                 "../managed_components/espressif__esp_jpeg"
                 "../managed_components/espressif__esp-tflite-micro"
                 "../managed_components/espressif__esp-nn"
                 "../managed_components/espressif__esp32-camera"

    REQUIRES esp_http_server
             esp_wifi
             esp_netif
             nvs_flash
             esp-tflite-micro
             esp32-camera                  
             
    PRIV_REQUIRES spi_flash
)
