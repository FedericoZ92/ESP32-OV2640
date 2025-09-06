idf_component_register(SRCS "camera_driver.cpp" "main.cpp"
                    PRIV_REQUIRES spi_flash
                    REQUIRES esp32-camera
                    INCLUDE_DIRS "")
