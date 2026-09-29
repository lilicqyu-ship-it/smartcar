# LVGL 9.6 marks lv_obj_add_flag/lv_obj_remove_flag deprecated and points to
# lv_obj_set_* setters that are not part of the pinned 9.6.0~1 component yet;
# silence the deprecation noise inside this component until the component ships
# the setters.
idf_component_register(
    SRCS
        "app_main.c"
        "app_state.c"
        "scr_settings.c"
        "scr_link.c"
        "scr_ctrl.c"
        "proto/proto_frames.c"
        "ui/ui.c"
        "ui/ui_theme.c"
        "ui/ui_joystick.c"
        "ui/ui_home.c"
        "ui/ui_pages.c"
        "ui/ui_alert.c"
    INCLUDE_DIRS "." "proto" "ui"
    REQUIRES
        nvs_flash
        esp_wifi
        esp_netif
        esp_event
        esp_timer
        esp_http_client
        esp_app_format
)

# LVGL 9.6 marks lv_obj_add_flag/lv_obj_remove_flag deprecated and points to
# lv_obj_set_* setters that are not part of the pinned 9.6.0~1 component yet;
# silence the deprecation noise inside this component until the component ships
# the setters.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-deprecated-declarations)
