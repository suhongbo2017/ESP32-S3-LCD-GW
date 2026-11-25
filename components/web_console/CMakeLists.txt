# ---------------------------------------------------------------------------
# web_console：本地 Web 控制台（esp_http_server + mDNS）
#  - GET  /                    状态页（内嵌 HTML）
#  - GET  /api/status          状态 JSON（上行/IP/温湿度/运行时长/MQTT/TF）
#  - GET  /api/gpio            已定义设备 / 可定义引脚 / 板载占用引脚
#  - POST /api/gpio/define     定义命名输出（JSON）
#  - POST /api/device?id=&value=0|1   设置命名设备开关
#  - GET  /api/logs            列出 TF 日志文件
#  - GET  /api/logs/<file>     下载 TF 日志
# ---------------------------------------------------------------------------
idf_component_register(
    SRCS "web_console.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_http_server mdns json app_update net_manager ha_mqtt tf_storage app_cfg sensor_history device_registry
             log esp_timer esp_common freertos
)