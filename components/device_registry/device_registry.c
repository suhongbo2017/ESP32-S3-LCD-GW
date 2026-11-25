/**
 * @file device_registry.c
 * @brief 运行时定义的命名 GPIO 设备注册表（NVS 持久化）
 */
#include "device_registry.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "device_registry";

#define DEVICE_MAX 16
#define DEV_NVS_NS "devdefs"
#define DEV_NVS_BUF 384

static device_t s_devs[DEVICE_MAX];
static int s_count = 0;
static SemaphoreHandle_t s_lock = NULL;
static device_change_cb_t s_change_cb = NULL;
static device_defined_cb_t s_define_cb = NULL;
static device_removed_cb_t s_remove_cb = NULL;

/** 板载占用引脚（与 PINMAP.md §3 对齐；不在白名单内的已用引脚） */
static const device_reserved_t s_reserved[] = {
    { 0,  "BOOT 按键（下载模式）" },
    { 10, "W5500 CS" },
    { 11, "W5500 MOSI" },
    { 12, "W5500 SCLK" },
    { 13, "W5500 MISO" },
    { 14, "W5500 中断" },
    { 15, "W5500/LCD 共用复位" },
    { 16, "LCD D/C" },
    { 17, "LCD MOSI" },
    { 18, "LCD SCLK" },
    { 19, "USB D-" },
    { 20, "USB D+" },
    { 21, "LCD CS" },
    { 26, "SPI Flash/PSRAM" },
    { 27, "SPI Flash/PSRAM" },
    { 28, "SPI Flash/PSRAM" },
    { 29, "SPI Flash/PSRAM" },
    { 30, "SPI Flash/PSRAM" },
    { 31, "SPI Flash/PSRAM" },
    { 32, "SPI Flash/PSRAM" },
    { 33, "TF 卡 DATA2" },
    { 34, "TF 卡 DATA3" },
    { 35, "TF 卡 CMD" },
    { 36, "TF 卡 CLK" },
    { 37, "TF 卡 DATA0" },
    { 38, "TF 卡 DATA1" },
    { 43, "UART0 TX" },
    { 44, "UART0 RX" },
    { 45, "VDD_SPI 电压选择" },
    { 46, "ROM 日志" },
};

bool device_pin_allowed(int pin)
{
    return (pin >= 1 && pin <= 9) || (pin >= 39 && pin <= 42) || pin == 47 || pin == 48;
}

int device_reserved_count(void)
{
    return (int)(sizeof(s_reserved) / sizeof(s_reserved[0]));
}

const device_reserved_t *device_reserved_at(int idx)
{
    if (idx < 0 || idx >= device_reserved_count()) {
        return NULL;
    }
    return &s_reserved[idx];
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

const device_t *device_find_pin(int pin)
{
    for (int i = 0; i < s_count; i++) {
        if (s_devs[i].pin == pin) {
            return &s_devs[i];
        }
    }
    return NULL;
}

/** 配置一个输出引脚并置为关闭 */
static void output_init(int pin)
{
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_INPUT_OUTPUT);
    gpio_set_level(pin, 0);
}

/** 写入 NVS：count + 每台设备的 JSON */
static esp_err_t persist_all(void)
{
    nvs_handle_t h;
    esp_err_t err = nvs_open(DEV_NVS_NS, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(h, "n", (uint8_t)s_count);
    for (int i = 0; err == ESP_OK && i < s_count; i++) {
        cJSON *o = cJSON_CreateObject();
        if (o == NULL) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        cJSON_AddStringToObject(o, "id", s_devs[i].id);
        cJSON_AddStringToObject(o, "name", s_devs[i].name);
        cJSON_AddStringToObject(o, "desc", s_devs[i].desc);
        cJSON_AddNumberToObject(o, "pin", s_devs[i].pin);
        char *json = cJSON_PrintUnformatted(o);
        cJSON_Delete(o);
        if (json == NULL) {
            err = ESP_ERR_NO_MEM;
            break;
        }
        char key[8];
        snprintf(key, sizeof(key), "d%d", i);
        err = nvs_set_str(h, key, json);
        cJSON_free(json);
    }
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}

/** 从 NVS 加载并登记一台设备（调用方持锁） */
static void load_one(const char *json)
{
    cJSON *o = cJSON_Parse(json);
    if (o == NULL) {
        return;
    }
    cJSON *pin = cJSON_GetObjectItem(o, "pin");
    cJSON *name = cJSON_GetObjectItem(o, "name");
    cJSON *desc = cJSON_GetObjectItem(o, "desc");
    if (!cJSON_IsNumber(pin) || !cJSON_IsString(name) || name->valuestring[0] == '\0' ||
        !device_pin_allowed(pin->valueint) || device_find_pin(pin->valueint) != NULL) {
        cJSON_Delete(o);
        return;
    }

    device_t *d = &s_devs[s_count++];
    memset(d, 0, sizeof(*d));
    snprintf(d->id, sizeof(d->id), "gpio%d", pin->valueint);
    snprintf(d->name, sizeof(d->name), "%s", name->valuestring);
    if (cJSON_IsString(desc)) {
        snprintf(d->desc, sizeof(d->desc), "%s", desc->valuestring);
    }
    d->kind = DEV_OUT;
    d->pin = pin->valueint;
    d->on = false;
    output_init(d->pin);
    ESP_LOGI(TAG, "恢复设备 %s（%s）-> GPIO%d", d->id, d->name, d->pin);
    cJSON_Delete(o);
}

esp_err_t device_registry_init(void)
{
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
    if (s_lock == NULL) {
        return ESP_ERR_NO_MEM;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_count = 0;

    nvs_handle_t h;
    if (nvs_open(DEV_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t n = 0;
        if (nvs_get_u8(h, "n", &n) == ESP_OK) {
            for (int i = 0; i < n && i < DEVICE_MAX; i++) {
                char key[8];
                char buf[DEV_NVS_BUF];
                size_t len = sizeof(buf);
                snprintf(key, sizeof(key), "d%d", i);
                if (nvs_get_str(h, key, buf, &len) == ESP_OK) {
                    load_one(buf);
                }
            }
        }
        nvs_close(h);
    }
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "已加载 %d 台自定义输出（共 %d 个板载占用引脚）",
             s_count, device_reserved_count());
    return ESP_OK;
}

esp_err_t device_define(const char *name, int pin, const char *desc)
{
    if (name == NULL || name[0] == '\0' || strlen(name) >= DEVICE_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (desc != NULL && strlen(desc) >= DEVICE_DESC_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (!device_pin_allowed(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_count >= DEVICE_MAX) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }
    if (device_find_pin(pin) != NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;
    }

    device_t *d = &s_devs[s_count];
    memset(d, 0, sizeof(*d));
    snprintf(d->id, sizeof(d->id), "gpio%d", pin);
    snprintf(d->name, sizeof(d->name), "%s", name);
    if (desc != NULL) {
        snprintf(d->desc, sizeof(d->desc), "%s", desc);
    }
    d->kind = DEV_OUT;
    d->pin = pin;
    d->on = false;
    output_init(pin);
    s_count++;

    esp_err_t err = persist_all();
    int saved_idx = s_count - 1;
    if (err != ESP_OK) {
        s_count--;
        gpio_set_level(pin, 0);
    }
    device_defined_cb_t cb = s_define_cb;
    device_t defined = s_devs[saved_idx];
    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "定义 GPIO%d 后写入 NVS 失败：%s", pin, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "已定义设备 %s（%s）-> GPIO%d", defined.id, defined.name, defined.pin);
    if (cb != NULL) {
        cb(&defined);
    }
    return ESP_OK;
}

esp_err_t device_remove(const char *id)
{
    if (id == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    int idx = -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_devs[i].id, id) == 0) {
            idx = i;
            break;
        }
    }
    if (idx < 0) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NOT_FOUND;
    }

    device_t removed = s_devs[idx];
    for (int i = idx; i < s_count - 1; i++) {
        s_devs[i] = s_devs[i + 1];
    }
    s_count--;
    memset(&s_devs[s_count], 0, sizeof(device_t));
    gpio_set_level(removed.pin, 0);     /* 删除即关闭输出 */

    esp_err_t err = persist_all();
    if (err != ESP_OK) {
        s_devs[s_count++] = removed;    /* 回滚（顺序可能变化，但定义不丢） */
    } else {
        nvs_handle_t h;
        if (nvs_open(DEV_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            char key[8];
            snprintf(key, sizeof(key), "d%d", s_count);
            nvs_erase_key(h, key);      /* 清掉已失效的最后一条旧记录 */
            nvs_commit(h);
            nvs_close(h);
        }
    }
    device_removed_cb_t cb = s_remove_cb;
    xSemaphoreGive(s_lock);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "删除 %s 后写入 NVS 失败：%s", id, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "已删除设备 %s（GPIO%d）", removed.id, removed.pin);
    if (cb != NULL) {
        cb(removed.id);
    }
    return ESP_OK;
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
    char idbuf[sizeof(d->id)];
    snprintf(idbuf, sizeof(idbuf), "%s", d->id);
    int pin = d->pin;
    device_change_cb_t cb = s_change_cb;
    xSemaphoreGive(s_lock);

    if (changed) {
        ESP_LOGI(TAG, "GPIO%d -> %s", pin, on ? "开" : "关");
        if (cb != NULL) {
            cb(idbuf, on);
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
            err = ESP_OK;
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

void device_registry_set_define_listener(device_defined_cb_t cb)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_define_cb = cb;
        xSemaphoreGive(s_lock);
    } else {
        s_define_cb = cb;
    }
}

void device_registry_set_remove_listener(device_removed_cb_t cb)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_remove_cb = cb;
        xSemaphoreGive(s_lock);
    } else {
        s_remove_cb = cb;
    }
}
