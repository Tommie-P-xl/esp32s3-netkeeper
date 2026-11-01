idf_component_register(
    SRCS
        app_main.c
        power_mgr.c
        db.c
        wifi_mgr.c
        net_guard.c
        web.c
        ui.c
        ui_common.c
        ui_setup.c
        ui_font_18.c
        srun_crypto.c
        srun_port.c
        sys_time.c
        http_util.c
        batt_mgr.c
    INCLUDE_DIRS .
    # 配置页构建时直接嵌进固件，板上不需要文件系统
    EMBED_FILES index.html
)
