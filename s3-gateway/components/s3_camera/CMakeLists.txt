idf_component_register(SRCS "camera_stream.c"
                       INCLUDE_DIRS "include"
                       REQUIRES esp_http_server esp_timer esp_driver_i2c
                       PRIV_REQUIRES esp32-camera esp_hw_support)
