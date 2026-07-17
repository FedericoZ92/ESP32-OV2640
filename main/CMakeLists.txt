idf_component_register(
    SRCS "wifi/wifi.cpp"
         "http-server/http-server.cpp"
         "http-server/http-frame-buffer.cpp"
         "camera-driver/camera-driver.cpp"
         "data-types/frame-mailbox.cpp"
         "tf-lite/tf-lite.cpp"
         "main.cpp"
         "tflite-person-detect/person_detect_model_data.cc"
         "image-editing/editing.cpp"
         "debug.cpp"
         "led/rgb-led.cpp"
         "led/red-led.cpp"
         "psram/psram.cpp"
         "checkpoint-timer/checkpoint-timer.cpp"
         "util/misc.cpp"

    INCLUDE_DIRS "."
                 "../managed_components/espressif__esp_jpeg"
                 "../managed_components/espressif__esp-tflite-micro"
                 "../managed_components/espressif__esp-nn"
                 "../managed_components/espressif__esp32-camera"

    REQUIRES esp_http_server
             esp_wifi
             esp_netif
             lwip
             nvs_flash
             esp-tflite-micro
             esp32-camera 
             esp_timer                   
             
    PRIV_REQUIRES spi_flash
)
