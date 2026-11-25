# ---------------------------------------------------------------------------
# devices：具体设备驱动（每加一个设备 = 加一个 .c + 一行注册）
#   dev_gpio_out：命名开关量输出（灯/风扇/继电器）
# ---------------------------------------------------------------------------
idf_component_register(
    SRCS "dev_gpio_out.c"
    INCLUDE_DIRS "include"
    REQUIRES device_registry driver log esp_common
)
