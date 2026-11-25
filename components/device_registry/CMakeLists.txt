# ---------------------------------------------------------------------------
# device_registry：命名 GPIO 设备注册表（Web 控件 + HA switch 共用）
# ---------------------------------------------------------------------------
idf_component_register(
    SRCS "device_registry.c"
    INCLUDE_DIRS "include"
    REQUIRES driver esp_driver_i2c esp_adc json nvs_flash log esp_timer esp_common freertos
)
