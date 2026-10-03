# LVGL 9.6 marks lv_obj_add_flag/lv_obj_remove_flag deprecated and points to
# lv_obj_set_* setters that are not part of the pinned 9.6.0~1 component yet;
# silence the deprecation noise inside this component until the component ships
# the setters.
set(SRCS
    "app_main.c"
    "app_state.c"
    "scr_settings.c"
    "scr_link.c"
    "scr_ctrl.c"
    "scr_svc.c"
    "scr_power.c"
    "lv_mem_psram.c"
    "proto/proto_frames.c"
    "ui/ui.c"
    "ui/ui_theme.c"
    "ui/ui_joystick.c"
    "ui/ui_home.c"
    "ui/ui_pages.c"
    "ui/ui_service.c"
    "ui/ui_alert.c"
)
set(PRIV_REQUIRES "")

# S3-gateway video/vision plane (CONFIG_SCR_S3GW_ENABLE, default n): the
# camera WS client and the two video pages are compiled out entirely when the
# remote runs against the C6 only.
if(CONFIG_SCR_S3GW_ENABLE)
    list(APPEND SRCS "scr_cam.c" "ui/ui_camera.c" "ui/ui_vision.c")
    list(APPEND PRIV_REQUIRES esp_jpeg esp_new_jpeg)
endif()

idf_component_register(
    SRCS ${SRCS}
    INCLUDE_DIRS "." "proto" "ui"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp_http_client
        esp_app_format
        esp_partition
        esp_rom
        lvgl__lvgl
    PRIV_REQUIRES ${PRIV_REQUIRES}
)

# LVGL 9.6 marks lv_obj_add_flag/lv_obj_remove_flag deprecated and points to
# lv_obj_set_* setters that are not part of the pinned 9.6.0~1 component yet;
# silence the deprecation noise inside this component until the component ships
# the setters.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-deprecated-declarations)

# lv_mem_psram.c provides LVGL's lv_*_core allocator hooks (LV_USE_CUSTOM_MALLOC).
# LVGL is linked after main, so pull the object in explicitly.
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_malloc_core")
