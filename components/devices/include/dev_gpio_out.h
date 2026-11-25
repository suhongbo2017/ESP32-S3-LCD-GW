/**
 * @file dev_gpio_out.h
 * @brief 命名 GPIO 输出设备（灯/风扇/继电器）注册入口
 */
#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 注册本板全部命名输出（必须在 device_registry_init() 之前调用） */
esp_err_t dev_gpio_out_register_all(void);

#ifdef __cplusplus
}
#endif
