idf_component_register(SRCS "app_main.c" "app_state.c"
                       INCLUDE_DIRS "include"
                       REQUIRES s3_proto s3_factory s3_link s3_net s3_http espcoredump
                                s3_pair s3_bridge s3_ota s3_led s3_camera nvs_flash
                                esp_timer esp_hw_support esp_wifi)

# optional components stay out of the default build graph until enabled
if(CONFIG_S3_MAINT_BLE)
    idf_component_get_property(lib s3_maint COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${lib})
endif()

if(CONFIG_S3_LEGACY_TCP)
    idf_component_get_property(lib2 s3_legacy COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PRIVATE ${lib2})
endif()
