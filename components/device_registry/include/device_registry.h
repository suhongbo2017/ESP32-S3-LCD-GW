/**
 * @file device_registry.h
 * @brief 运行时定义的命名设备注册表（开关输出 / I2C 传感器 / ADC 模拟量）
 *
 * 定义保存在 NVS：设备名、说明、类型、引脚/地址。上电自动恢复。
 * 支持：DEV_OUT（继电器）、DEV_I2C_SENSOR（SHT31/SHT4x/BH1750）、
 *       DEV_ADC（声音包络等模拟量，ADC1）。
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DEVICE_NAME_MAX 40      /*!< 设备名（UTF-8 字节，含 NUL） */
#define DEVICE_DESC_MAX 96      /*!< 说明（UTF-8 字节，含 NUL） */
#define DEVICE_METRICS_MAX 2    /*!< 每台设备最多两个指标（温+湿） */

/** 设备类型 */
typedef enum {
    DEV_OUT = 0,        /*!< 开关量输出 → Web 开关 + HA switch */
    DEV_ADC = 1,        /*!< ADC 模拟量（声音包络等）→ HA sensor */
    DEV_I2C_SENSOR = 2, /*!< I2C 传感器 → HA sensor（可多指标） */
    DEV_DHT = 3,        /*!< DHT22/AM2302 单总线温湿度 */
} dev_kind_t;

/** I2C 传感器型号 */
typedef enum {
    SENSOR_MODEL_NONE = 0,
    SENSOR_SHT31 = 1,   /*!< 温湿度，默认地址 0x44 */
    SENSOR_SHT4X = 2,   /*!< 温湿度，默认地址 0x44 */
    SENSOR_BH1750 = 3,  /*!< 光照，默认地址 0x23 */
} sensor_model_t;

/** 设备描述（由注册表持有） */
typedef struct {
    char        id[16];                /*!< 自动生成：gpio<N>/adc<N>/i2c<addr> */
    char        name[DEVICE_NAME_MAX];
    char        desc[DEVICE_DESC_MAX];
    dev_kind_t  kind;
    int         pin;                   /*!< DEV_OUT / DEV_ADC 使用 */
    bool        on;                    /*!< DEV_OUT 当前状态 */
    uint8_t     model;                 /*!< sensor_model_t（I2C） */
    uint8_t     addr;                  /*!< I2C 7 位地址 */
    bool        valid[DEVICE_METRICS_MAX];
    float       value[DEVICE_METRICS_MAX];
    int64_t     value_us;              /*!< 最近一次成功采样时刻 */
    int64_t     last_try_us;           /*!< 最近一次尝试采样时刻（节流用） */
    uint16_t    interval_ms;           /*!< 采样周期 */
    void       *drv;                   /*!< 驱动私有句柄（勿直接使用） */
} device_t;

/** 定义新设备的入参 */
typedef struct {
    const char     *name;
    const char     *desc;
    dev_kind_t      kind;
    int             pin;               /*!< DEV_OUT / DEV_ADC */
    sensor_model_t  model;             /*!< DEV_I2C_SENSOR */
    uint8_t         addr;              /*!< 0 = 型号默认地址 */
} dev_def_t;

/** 板载占用引脚（网页“不可定义”说明窗用） */
typedef struct {
    int         pin;
    const char *reason;
} device_reserved_t;

/** 开关输出状态变化回调 */
typedef void (*device_change_cb_t)(const char *id, bool on);

/** 新设备定义回调（HTTP 任务上下文） */
typedef void (*device_defined_cb_t)(const device_t *dev);

/** 设备删除回调（HTTP 任务上下文，传入被删除设备的副本） */
typedef void (*device_removed_cb_t)(const device_t *dev);

/** 传感器采样更新回调（轮询任务上下文，传入只读副本） */
typedef void (*device_value_cb_t)(const device_t *dev);

/** 初始化：恢复 NVS 定义、初始化 I2C/ADC、启动采样任务 */
esp_err_t device_registry_init(void);

/** 定义一个设备并持久化到 NVS */
esp_err_t device_define(const dev_def_t *def);

/** 删除一个设备：关闭输出、释放 I2C 句柄、从 NVS 清除 */
esp_err_t device_remove(const char *id);

int device_count(void);
const device_t *device_at(int idx);
const device_t *device_find(const char *id);
const device_t *device_find_pin(int pin);

/** 设置 DEV_OUT 状态；状态变化时触发回调 */
esp_err_t device_set(const char *id, bool on);

/** 读取 DEV_OUT 状态 */
esp_err_t device_get(const char *id, bool *on);

/** 数字输出引脚是否允许（GPIO1–9 / 39–42 / 47–48） */
bool device_pin_allowed(int pin);

/** ADC1 引脚是否允许（GPIO1–9；ESP32-S3 ADC2 在 WiFi 下不可用） */
bool device_adc_pin_allowed(int pin);

/** 指标元数据：数量 / MQTT key / 显示名 / 单位 */
int device_metric_count(const device_t *d);
const char *device_metric_key(const device_t *d, int idx);
const char *device_metric_label(const device_t *d, int idx);
const char *device_metric_unit(const device_t *d, int idx);

/** I2C 总线配置（未初始化时 ready=false） */
esp_err_t device_bus_get(int *sda, int *scl, bool *ready);
esp_err_t device_bus_set(int sda, int scl);

/** 扫描 I2C 总线（8..0x77），返回发现数量；总线不可用返回 -1 */
int device_bus_scan(uint8_t *addrs, int max);

/** DHT22 诊断结果（接线/时序排查） */
typedef struct {
    int     idle_level;          /*!< 起始前总线电平（应为 1） */
    int     drive_low_level;     /*!< 驱动低电平时读回（应为 0） */
    int     release_level;       /*!< 释放后读回（应为 1） */
    int     setup_us;            /*!< 进入临界区到释放的耗时 */
    int     resp_low_us;         /*!< 传感器应答低电平时长；-1=超时 */
    int     resp_high_us;        /*!< 应答高电平时长；-1=超时 */
    int     bit_high_us[40];     /*!< 每个 bit 的高电平时长；-1=超时 */
    uint8_t data[5];
    bool    checksum_ok;
    float   temp;
    float   hum;
} device_dht_debug_t;

/** 对指定引脚做一次 DHT22 读取并返回详细时序（不修改设备表） */
esp_err_t device_dht_debug(int pin, device_dht_debug_t *out);

/** 取任一有效的本机温湿度传感器（DHT/SHT）；没有返回 false */
bool device_get_temp_hum(float *temp, float *hum);

/** 板载占用引脚列表 */
int device_reserved_count(void);
const device_reserved_t *device_reserved_at(int idx);

void device_registry_set_change_listener(device_change_cb_t cb);
void device_registry_set_define_listener(device_defined_cb_t cb);
void device_registry_set_remove_listener(device_removed_cb_t cb);
void device_registry_set_value_listener(device_value_cb_t cb);

#ifdef __cplusplus
}
#endif
