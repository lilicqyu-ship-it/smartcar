idf_component_register(SRCS "bridge.c"
                       INCLUDE_DIRS "include"
                       REQUIRES s3_proto s3_link s3_http s3_pair s3_camera esp_timer)
