/**
 * @file device_registry.h
 * @brief 命名 GPIO 设备注册表（固定引脚，网页与 HA 共用同一张表）
 *
 * 设计见 DEVICES.md §3：加设备 = 在 devices 组件里写一行注册，Web 控件和
 * HA Discovery/MQTT 自动获得，无需改 web_console / ha_mqtt。
 *
 * 当前仅实现 DEV_OUT（低压开关量输出）。所有输出引脚必须在白名单内，
 * 上电默认关闭（低电平）。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 设备类型（后续类型按 DEVICES.md 逐步实现） */
typedef enum {
    DEV_OUT = 0,   /*!< 开关量输出：继电器/LED → Web 开关 + HA switch */
    DEV_IN,        /*!< 开关量输入（未实现） */
    DEV_ADC,       /*!< 模拟量输入（未实现） */
    DEV_PWM,       /*!< PWM 输出（未实现） */
    DEV_SENSOR,    /*!< 复合传感器（未实现） */
} dev_kind_t;

/** 设备描述符（注册时拷贝；on 由注册表维护） */
typedef struct {
    const char *id;    /*!< 唯一标识（ASCII，用于 URL/MQTT 主题），如 "light" */
    const char *name;  /*!< 显示名（可中文），如 "箱内灯" */
    dev_kind_t  kind;  /*!< 设备类型 */
    int         pin;   /*!< 主引脚（DEV_OUT 必须命中白名单） */
    bool        on;    /*!< 当前状态（DEV_OUT）；只读，勿直接改 */
} device_t;

/** 状态变化回调：由注册表在写操作成功后调用（不在持锁上下文） */
typedef void (*device_change_cb_t)(const char *id, bool on);

/** 注册一个设备（须在 device_registry_init() 之前调用） */
esp_err_t device_register(const device_t *dev);

/** 初始化已注册设备：校验引脚、配置输出并置为关闭（幂等） */
esp_err_t device_registry_init(void);

/** 已注册设备数量 */
int device_count(void);

/** 按下标取设备（越界返回 NULL） */
const device_t *device_at(int idx);

/** 按 id 查找设备（未找到返回 NULL） */
const device_t *device_find(const char *id);

/** 设置 DEV_OUT 状态；状态变化时触发回调 */
esp_err_t device_set(const char *id, bool on);

/** 读取 DEV_OUT 状态 */
esp_err_t device_get(const char *id, bool *on);

/** 引脚是否允许用作输出（GPIO1–9 / 39–42 / 47–48） */
bool device_pin_allowed(int pin);

/** 注册状态变化监听（传 NULL 取消）；用于 HA 发布状态 */
void device_registry_set_change_listener(device_change_cb_t cb);

#ifdef __cplusplus
}
#endif
