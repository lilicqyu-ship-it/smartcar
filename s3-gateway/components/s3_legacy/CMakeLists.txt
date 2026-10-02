# TCP 8080 legacy bridge - compiled only when CONFIG_S3_LEGACY_TCP is enabled
if(CONFIG_S3_LEGACY_TCP)
    idf_component_register(SRCS "legacy_tcp.c"
                           INCLUDE_DIRS "include"
                           REQUIRES s3_link s3_proto lwip esp_timer)
else()
    idf_component_register()
endif()
