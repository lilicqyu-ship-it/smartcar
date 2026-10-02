idf_component_register(SRCS "pair.c"
                       INCLUDE_DIRS "include"
                       REQUIRES s3_proto s3_factory mbedtls esp_timer esp_hw_support)
