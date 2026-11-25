/**
 * @file device_registry.c
 * @brief 命名 GPIO 设备注册表实现（见 device_registry.h）
 */
#include "device_registry.h"

#include <string.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "device_registry";

#define DEVICE_MAX 16

static device_t s_devs[DEVICE_MAX];
static int s_count = 0;
static SemaphoreHandle_t s_lock = NULL;
static device_change_cb_t s_change_cb = NULL;

bool device_pin_allowed(int pin)
{
    return (pin >= 1 && pin <= 9) || (pin >= 39 && pin <= 42) || pin == 47 || pin == 48;
}

esp_err_t device_register(const device_t *dev)
{
    if (dev == NULL || dev->id == NULL || dev->id[0] == '\0' ||
        dev->name == NULL || dev->name[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (dev->kind == DEV_OUT && !device_pin_allowed(dev->pin)) {
        ESP_LOGE(TAG, "设备 %s 的引脚 GPIO%d 不在白名单", dev->id, dev->pin);
        return ESP_ERR_INVALID_ARG;
    }
    if (s_count >= DEVICE_MAX) {
        return ESP_ERR_NO_MEM;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_devs[i].id, dev->id) == 0) {
            ESP_LOGE(TAG, "设备 id 重复：%s", dev->id);
            return ESP_ERR_INVALID_STATE;
        }
    }

    s_devs[s_count] = *dev;
    s_devs[s_count].on = false;    /* 上电默认关闭 */
    s_count++;
    return ESP_OK;
}

esp_err_t device_registry_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    for (int i = 0; i < s_count; i++) {
        device_t *d = &s_devs[i];
        if (d->kind != DEV_OUT) {
            continue;
        }
        if (!device_pin_allowed(d->pin)) {
            ESP_LOGE(TAG, "设备 %s 的引脚 GPIO%d 不在白名单，已跳过", d->id, d->pin);
            continue;
        }

        gpio_reset_pin(d->pin);
        gpio_set_direction(d->pin, GPIO_MODE_INPUT_OUTPUT);
        gpio_set_level(d->pin, 0);
        d->on = false;
        ESP_LOGI(TAG, "设备 %s（%s）-> GPIO%d，默认关闭", d->id, d->name, d->pin);
    }
    return ESP_OK;
}

int device_count(void)
{
    return s_count;
}

const device_t *device_at(int idx)
{
    if (idx < 0 || idx >= s_count) {
        return NULL;
    }
    return &s_devs[idx];
}

const device_t *device_find(const char *id)
{
    if (id == NULL) {
        return NULL;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_devs[i].id, id) == 0) {
            return &s_devs[i];
        }
    }
    return NULL;
}

esp_err_t device_set(const char *id, bool on)
{
    if (id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    device_t *d = NULL;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_devs[i].id, id) == 0) {
            d = &s_devs[i];
            break;
        }
    }
    if (d == NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }
    if (d->kind != DEV_OUT) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }

    bool changed = (d->on != on);
    if (changed) {
        gpio_set_level(d->pin, on ? 1 : 0);
        d->on = on;
    }
    device_change_cb_t cb = s_change_cb;
    xSemaphoreGive(s_lock);

    if (changed) {
        ESP_LOGI(TAG, "%s（GPIO%d）-> %s", d->name, d->pin, on ? "开" : "关");
        if (cb != NULL) {
            cb(d->id, on);
        }
    }
    return ESP_OK;
}

esp_err_t device_get(const char *id, bool *on)
{
    if (id == NULL || on == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    esp_err_t err = ESP_ERR_NOT_FOUND;
    xSemaphoreTake(s_lock, portMAX_DELAY);
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_devs[i].id, id) == 0) {
            *on = s_devs[i].on;
            err = (s_devs[i].kind == DEV_OUT) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

void device_registry_set_change_listener(device_change_cb_t cb)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_change_cb = cb;
        xSemaphoreGive(s_lock);
    } else {
        s_change_cb = cb;
    }
}
