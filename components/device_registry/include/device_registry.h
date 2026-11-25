/**
 * @file device_registry.h
 * @brief 运行时定义的命名 GPIO 设备注册表（Web 定义，HA switch 同步）
 *
 * 定义保存在 NVS：设备名、引脚、说明。上电自动恢复并默认关闭。
 * 输出引脚必须在白名单内；板载占用引脚由 device_reserved_* 提供给网页展示。
 */
#pragma once

#include <stdbool.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_NAME_MAX 40   /*!< 设备名（UTF-8 字节，含结尾 NUL） */
#define DEVICE_DESC_MAX 96   /*!< 说明（UTF-8 字节，含结尾 NUL） */

/** 设备类型（当前仅实现 DEV_OUT） */
typedef enum {
    DEV_OUT = 0,   /*!< 开关量输出：继电器/LED → Web 开关 + HA switch */
    DEV_IN,        /*!< 开关量输入（未实现） */
    DEV_ADC,       /*!< 模拟量输入（未实现） */
    DEV_PWM,       /*!< PWM 输出（未实现） */
    DEV_SENSOR,    /*!< 复合传感器（未实现） */
} dev_kind_t;

/** 设备描述（由注册表持有） */
typedef struct {
    char        id[16];               /*!< 自动生成：gpio<N>，用于 URL/MQTT 主题 */
    char        name[DEVICE_NAME_MAX];/*!< 用户定义的设备名 */
    char        desc[DEVICE_DESC_MAX];/*!< 用户定义的说明 */
    dev_kind_t  kind;                 /*!< 设备类型 */
    int         pin;                  /*!< 输出引脚 */
    bool        on;                   /*!< 当前状态；只读，勿直接改 */
} device_t;

/** 板载占用引脚（网页“不可定义”说明窗用） */
typedef struct {
    int         pin;
    const char *reason;
} device_reserved_t;

/** 状态变化回调：由注册表在写操作成功后调用（不在持锁上下文） */
typedef void (*device_change_cb_t)(const char *id, bool on);

/** 新设备定义回调：在 HTTP 任务上下文调用，用于立即发布 HA Discovery */
typedef void (*device_defined_cb_t)(const device_t *dev);

/** 初始化：从 NVS 恢复定义并配置输出（未定义的引脚保持原状） */
esp_err_t device_registry_init(void);

/**
 * @brief 运行时定义一个输出设备并持久化到 NVS
 *
 * @param name 设备名（非空，< DEVICE_NAME_MAX 字节）
 * @param pin  引脚（必须在白名单且未被占用）
 * @param desc 说明（可为 NULL/空）
 */
esp_err_t device_define(const char *name, int pin, const char *desc);

int device_count(void);
const device_t *device_at(int idx);
const device_t *device_find(const char *id);
const device_t *device_find_pin(int pin);

/** 设置 DEV_OUT 状态；状态变化时触发回调 */
esp_err_t device_set(const char *id, bool on);

/** 读取 DEV_OUT 状态 */
esp_err_t device_get(const char *id, bool *on);

/** 引脚是否允许用作输出（GPIO1–9 / 39–42 / 47–48） */
bool device_pin_allowed(int pin);

/** 板载占用引脚列表（说明窗数据源） */
int device_reserved_count(void);
const device_reserved_t *device_reserved_at(int idx);

/** 注册状态变化监听（传 NULL 取消）；用于 HA 发布状态 */
void device_registry_set_change_listener(device_change_cb_t cb);

/** 注册新定义监听（传 NULL 取消）；用于 HA 立即发布 Discovery */
void device_registry_set_define_listener(device_defined_cb_t cb);

#ifdef __cplusplus
}
#endif
