/**
 * @file dev_gpio_out.c
 * @brief 本板的命名开关量输出表
 *
 * 加/改按钮只改下面这张表：Web 控制台与 Home Assistant 的 switch 会自动跟随。
 * 引脚必须在白名单内（GPIO1–9 / 39–42 / 47–48）；上电默认关闭（低电平）。
 * 若继电器为高电平触发，请加反相驱动，勿直接改这里为默认高。
 */
#include "dev_gpio_out.h"

#include <stddef.h>

#include "device_registry.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "dev_gpio_out";

/* id（ASCII，MQTT/URL 用）     显示名         引脚 */
static const device_t s_outputs[] = {
    { .id = "light", .name = "箱内灯",     .kind = DEV_OUT, .pin = 4 },
    { .id = "fan",   .name = "排风扇",     .kind = DEV_OUT, .pin = 5 },
    { .id = "aux1",  .name = "备用输出 1", .kind = DEV_OUT, .pin = 6 },
    { .id = "aux2",  .name = "备用输出 2", .kind = DEV_OUT, .pin = 7 },
};

esp_err_t dev_gpio_out_register_all(void)
{
    for (size_t i = 0; i < sizeof(s_outputs) / sizeof(s_outputs[0]); i++) {
        ESP_RETURN_ON_ERROR(device_register(&s_outputs[i]), TAG,
                            "注册输出设备失败：%s", s_outputs[i].id);
    }
    ESP_LOGI(TAG, "已注册 %u 个命名输出", (unsigned)(sizeof(s_outputs) / sizeof(s_outputs[0])));
    return ESP_OK;
}
