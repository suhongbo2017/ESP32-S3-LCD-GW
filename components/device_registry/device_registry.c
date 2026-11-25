/**
 * @file device_registry.c
 * @brief 运行时定义的命名设备注册表（NVS 持久化；I2C/ADC 传感器轮询）
 */
#include "device_registry.h"

#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "device_registry";

#define DEVICE_MAX 16
#define DEV_NVS_NS "devdefs"
#define DEV_BUS_NS "devbus"
#define DEV_NVS_BUF 384
#define POLL_PERIOD_MS 1000
#define POLL_TASK_STACK 6144
#define POLL_TASK_PRIO 4

#define I2C_DEFAULT_SDA 47
#define I2C_DEFAULT_SCL 48
#define I2C_TIMEOUT_MS 50

static device_t s_devs[DEVICE_MAX];
static int s_count = 0;
static SemaphoreHandle_t s_lock = NULL;
static TaskHandle_t s_poll_task = NULL;
static device_change_cb_t s_change_cb = NULL;
static device_defined_cb_t s_define_cb = NULL;
static device_removed_cb_t s_remove_cb = NULL;
static device_value_cb_t s_value_cb = NULL;

static i2c_master_bus_handle_t s_bus = NULL;
static int s_bus_sda = I2C_DEFAULT_SDA;
static int s_bus_scl = I2C_DEFAULT_SCL;
static adc_oneshot_unit_handle_t s_adc = NULL;
static adc_cali_handle_t s_cali = NULL;

static void s_poll_task_fn(void *arg);

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

bool device_adc_pin_allowed(int pin)
{
    /* ESP32-S3 ADC1 = GPIO1..GPIO10；ADC2 与 WiFi 冲突，只开放 1-9 */
    return pin >= 1 && pin <= 9;
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
        if ((s_devs[i].kind == DEV_OUT || s_devs[i].kind == DEV_ADC ||
             s_devs[i].kind == DEV_DHT) && s_devs[i].pin == pin) {
            return &s_devs[i];
        }
    }
    return NULL;
}

/* ------------------------------------------------------------------ */
/* 指标元数据                                                          */
/* ------------------------------------------------------------------ */

static uint8_t default_addr(sensor_model_t model)
{
    switch (model) {
    case SENSOR_SHT31:
    case SENSOR_SHT4X:
        return 0x44;
    case SENSOR_BH1750:
        return 0x23;
    default:
        return 0;
    }
}

int device_metric_count(const device_t *d)
{
    if (d == NULL) {
        return 0;
    }
    if (d->kind == DEV_ADC) {
        return 1;
    }
    if (d->kind == DEV_DHT) {
        return 2;
    }
    if (d->kind == DEV_I2C_SENSOR) {
        return (d->model == SENSOR_SHT31 || d->model == SENSOR_SHT4X) ? 2 : 1;
    }
    return 0;
}

const char *device_metric_key(const device_t *d, int idx)
{
    if (d == NULL || idx < 0 || idx >= device_metric_count(d)) {
        return "";
    }
    if (d->kind == DEV_ADC) {
        return "level";
    }
    if (d->kind == DEV_DHT || d->model == SENSOR_SHT31 || d->model == SENSOR_SHT4X) {
        return idx == 0 ? "temp" : "hum";
    }
    return "lux";
}

const char *device_metric_label(const device_t *d, int idx)
{
    if (d == NULL || idx < 0 || idx >= device_metric_count(d)) {
        return "";
    }
    if (d->kind == DEV_ADC) {
        return "声音强度";
    }
    if (d->kind == DEV_DHT || d->model == SENSOR_SHT31 || d->model == SENSOR_SHT4X) {
        return idx == 0 ? "温度" : "湿度";
    }
    return "照度";
}

const char *device_metric_unit(const device_t *d, int idx)
{
    if (d == NULL || idx < 0 || idx >= device_metric_count(d)) {
        return "";
    }
    if (d->kind == DEV_ADC) {
        return "mV";
    }
    if (d->kind == DEV_DHT || d->model == SENSOR_SHT31 || d->model == SENSOR_SHT4X) {
        return idx == 0 ? "℃" : "%";
    }
    return "lx";
}

/* ------------------------------------------------------------------ */
/* I2C 总线与驱动                                                       */
/* ------------------------------------------------------------------ */

static uint8_t crc8(const uint8_t *data, int len)
{
    uint8_t crc = 0xFF;
    for (int i = 0; i < len; i++) {
        crc ^= data[i];
        for (int b = 0; b < 8; b++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static bool i2c_read(device_t *d);

static esp_err_t i2c_attach(device_t *d)
{
    if (s_bus == NULL || d->kind != DEV_I2C_SENSOR) {
        return ESP_ERR_INVALID_STATE;
    }
    i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = d->addr,
        .scl_speed_hz = 100000,
    };
    i2c_master_dev_handle_t dev = NULL;
    esp_err_t err = i2c_master_bus_add_device(s_bus, &cfg, &dev);
    if (err != ESP_OK) {
        return err;
    }
    d->drv = dev;

    if (d->model == SENSOR_BH1750) {
        uint8_t cmd = 0x10;   /* continuous H-resolution */
        if (i2c_master_transmit(dev, &cmd, 1, I2C_TIMEOUT_MS) != ESP_OK) {
            ESP_LOGW(TAG, "%s：BH1750 无应答（addr=0x%02X）", d->id, d->addr);
            i2c_master_bus_rm_device(dev);
            d->drv = NULL;
            return ESP_ERR_NOT_FOUND;
        }
        vTaskDelay(pdMS_TO_TICKS(180));
    }

    /* 探测一次，确认传感器真的存在且协议匹配 */
    if (!i2c_read(d)) {
        ESP_LOGW(TAG, "%s：传感器无有效读数（addr=0x%02X）", d->id, d->addr);
        i2c_master_bus_rm_device(dev);
        d->drv = NULL;
        return ESP_ERR_NOT_FOUND;
    }
    return ESP_OK;
}

static void i2c_detach(device_t *d)
{
    if (d->kind == DEV_I2C_SENSOR && d->drv != NULL) {
        i2c_master_bus_rm_device((i2c_master_dev_handle_t)d->drv);
        d->drv = NULL;
    }
}

static void i2c_bus_deinit(void)
{
    for (int i = 0; i < s_count; i++) {
        i2c_detach(&s_devs[i]);
    }
    if (s_bus != NULL) {
        i2c_del_master_bus(s_bus);
        s_bus = NULL;
    }
}

static esp_err_t i2c_bus_init(void)
{
    i2c_master_bus_config_t cfg = {
        .i2c_port = I2C_NUM_0,
        .sda_io_num = s_bus_sda,
        .scl_io_num = s_bus_scl,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    esp_err_t err = i2c_new_master_bus(&cfg, &s_bus);
    if (err != ESP_OK) {
        s_bus = NULL;
        ESP_LOGE(TAG, "I2C 总线初始化失败（SDA=%d SCL=%d）：%s",
                 s_bus_sda, s_bus_scl, esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "I2C 总线就绪：SDA=%d SCL=%d", s_bus_sda, s_bus_scl);

    for (int i = 0; i < s_count; i++) {
        if (s_devs[i].kind == DEV_I2C_SENSOR) {
            if (i2c_attach(&s_devs[i]) != ESP_OK) {
                ESP_LOGW(TAG, "%s：I2C 设备挂载失败（addr=0x%02X）",
                         s_devs[i].id, s_devs[i].addr);
            }
        }
    }
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* ADC                                                                 */
/* ------------------------------------------------------------------ */

static void adc_init(void)
{
    adc_oneshot_unit_init_cfg_t unit_cfg = {
        .unit_id = ADC_UNIT_1,
        .ulp_mode = ADC_ULP_MODE_DISABLE,
    };
    if (adc_oneshot_new_unit(&unit_cfg, &s_adc) != ESP_OK) {
        ESP_LOGE(TAG, "ADC1 初始化失败");
        s_adc = NULL;
        return;
    }
    adc_cali_curve_fitting_config_t cali_cfg = {
        .unit_id = ADC_UNIT_1,
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    if (adc_cali_create_scheme_curve_fitting(&cali_cfg, &s_cali) != ESP_OK) {
        ESP_LOGW(TAG, "ADC 校准不可用，使用线性换算");
        s_cali = NULL;
    }
}

static esp_err_t adc_attach(device_t *d)
{
    if (s_adc == NULL || !device_adc_pin_allowed(d->pin)) {
        return ESP_ERR_INVALID_STATE;
    }
    adc_oneshot_chan_cfg_t cfg = {
        .atten = ADC_ATTEN_DB_12,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    return adc_oneshot_config_channel(s_adc, (adc_channel_t)(d->pin - 1), &cfg);
}

static bool adc_read(device_t *d)
{
    if (s_adc == NULL) {
        d->valid[0] = false;
        return false;
    }
    int ch = d->pin - 1;
    int mn = 4095, mx = 0;
    for (int i = 0; i < 24; i++) {
        int raw = 0;
        if (adc_oneshot_read(s_adc, (adc_channel_t)ch, &raw) != ESP_OK) {
            d->valid[0] = false;
            return false;
        }
        if (raw < mn) mn = raw;
        if (raw > mx) mx = raw;
        esp_rom_delay_us(200);
    }
    int mv_min = mn * 3300 / 4095;
    int mv_max = mx * 3300 / 4095;
    if (s_cali != NULL) {
        int v1 = mv_min, v2 = mv_max;
        if (adc_cali_raw_to_voltage(s_cali, mn, &v1) == ESP_OK &&
            adc_cali_raw_to_voltage(s_cali, mx, &v2) == ESP_OK) {
            mv_min = v1;
            mv_max = v2;
        }
    }
    d->value[0] = (float)(mv_max - mv_min);
    d->valid[0] = true;
    d->value_us = esp_timer_get_time();
    return true;
}

/* ------------------------------------------------------------------ */
/* DHT22 / AM2302 单总线                                               */
/* ------------------------------------------------------------------ */

/** 等待引脚到达指定电平（微秒级；仅在临界区内调用） */
static bool dht_wait_level(int pin, int level, int timeout_us)
{
    for (int i = 0; i < timeout_us; i++) {
        if (gpio_get_level(pin) == level) {
            return true;
        }
        esp_rom_delay_us(1);
    }
    return false;
}

static void dht_attach(device_t *d)
{
    gpio_reset_pin(d->pin);
    gpio_set_direction(d->pin, GPIO_MODE_INPUT_OUTPUT_OD);
    gpio_set_pull_mode(d->pin, GPIO_PULLUP_ONLY);
    gpio_set_level(d->pin, 1);      /* 释放总线 */
    d->interval_ms = 2500;
}

static bool dht_read(device_t *d)
{
    const int pin = d->pin;
    uint8_t data[5] = { 0 };

    gpio_set_level(pin, 0);                 /* 起始信号：拉低 2ms */
    vTaskDelay(pdMS_TO_TICKS(2));
    gpio_set_level(pin, 1);

    portMUX_TYPE mux = portMUX_INITIALIZER_UNLOCKED;
    bool ok = false;
    taskENTER_CRITICAL(&mux);
    if (dht_wait_level(pin, 0, 120) &&      /* 传感器应答低 80us */
        dht_wait_level(pin, 1, 120) &&      /* 高 80us */
        dht_wait_level(pin, 0, 120)) {      /* 第一位起始低 */
        ok = true;
        for (int i = 0; i < 40 && ok; i++) {
            if (!dht_wait_level(pin, 1, 120)) { ok = false; break; }
            esp_rom_delay_us(40);           /* 26-28us=0，70us=1 */
            int v = gpio_get_level(pin);
            data[i / 8] = (uint8_t)((data[i / 8] << 1) | (v ? 1 : 0));
            if (!dht_wait_level(pin, 0, 120)) { ok = false; break; }
        }
    }
    taskEXIT_CRITICAL(&mux);

    if (!ok || (uint8_t)(data[0] + data[1] + data[2] + data[3]) != data[4]) {
        d->valid[0] = d->valid[1] = false;
        return false;
    }
    uint16_t hum = (uint16_t)((data[0] << 8) | data[1]);
    uint16_t temp = (uint16_t)(((data[2] & 0x7F) << 8) | data[3]);
    d->value[0] = temp / 10.0f * ((data[2] & 0x80) ? -1.0f : 1.0f);
    d->value[1] = hum / 10.0f;
    d->valid[0] = d->valid[1] = true;
    d->value_us = esp_timer_get_time();
    return true;
}

/* ------------------------------------------------------------------ */
/* I2C 采样                                                            */
/* ------------------------------------------------------------------ */

static bool i2c_read(device_t *d)
{
    if (d->drv == NULL) {
        return false;
    }
    i2c_master_dev_handle_t dev = (i2c_master_dev_handle_t)d->drv;
    uint8_t buf[6] = { 0 };

    if (d->model == SENSOR_SHT31 || d->model == SENSOR_SHT4X) {
        uint8_t cmd[2];
        if (d->model == SENSOR_SHT31) {
            cmd[0] = 0x2C; cmd[1] = 0x06;    /* high repeatability, no clock stretch */
        } else {
            cmd[0] = 0xFD;                    /* SHT4x high precision */
        }
        if (i2c_master_transmit(dev, cmd, (d->model == SENSOR_SHT31) ? 2 : 1,
                                I2C_TIMEOUT_MS) != ESP_OK) {
            d->valid[0] = d->valid[1] = false;
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(d->model == SENSOR_SHT31 ? 20 : 10));
        if (i2c_master_receive(dev, buf, 6, I2C_TIMEOUT_MS) != ESP_OK ||
            crc8(buf, 2) != buf[2] || crc8(buf + 3, 2) != buf[5]) {
            d->valid[0] = d->valid[1] = false;
            return false;
        }
        uint16_t traw = (uint16_t)((buf[0] << 8) | buf[1]);
        uint16_t hraw = (uint16_t)((buf[3] << 8) | buf[4]);
        if (d->model == SENSOR_SHT31) {
            d->value[0] = -45.0f + 175.0f * (float)traw / 65535.0f;
            d->value[1] = 100.0f * (float)hraw / 65535.0f;
        } else {
            d->value[0] = -45.0f + 175.0f * (float)traw / 65535.0f;
            d->value[1] = -6.0f + 125.0f * (float)hraw / 65535.0f;
        }
        d->valid[0] = d->valid[1] = true;
        d->value_us = esp_timer_get_time();
        return true;
    }

    if (d->model == SENSOR_BH1750) {
        if (i2c_master_receive(dev, buf, 2, I2C_TIMEOUT_MS) != ESP_OK) {
            d->valid[0] = false;
            return false;
        }
        uint16_t raw = (uint16_t)((buf[0] << 8) | buf[1]);
        d->value[0] = (float)raw / 1.2f;
        d->valid[0] = true;
        d->value_us = esp_timer_get_time();
        return true;
    }
    return false;
}

/* ------------------------------------------------------------------ */
/* NVS                                                                 */
/* ------------------------------------------------------------------ */

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
        cJSON_AddNumberToObject(o, "kind", s_devs[i].kind);
        cJSON_AddNumberToObject(o, "pin", s_devs[i].pin);
        cJSON_AddNumberToObject(o, "model", s_devs[i].model);
        cJSON_AddNumberToObject(o, "addr", s_devs[i].addr);
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

static void load_device(const char *json)
{
    cJSON *o = cJSON_Parse(json);
    if (o == NULL) {
        return;
    }
    cJSON *pin = cJSON_GetObjectItem(o, "pin");
    cJSON *name = cJSON_GetObjectItem(o, "name");
    cJSON *desc = cJSON_GetObjectItem(o, "desc");
    cJSON *kind = cJSON_GetObjectItem(o, "kind");
    cJSON *model = cJSON_GetObjectItem(o, "model");
    cJSON *addr = cJSON_GetObjectItem(o, "addr");
    if (!cJSON_IsString(name) || name->valuestring[0] == '\0') {
        cJSON_Delete(o);
        return;
    }

    dev_def_t def = { 0 };
    def.name = name->valuestring;
    def.desc = cJSON_IsString(desc) ? desc->valuestring : "";
    def.kind = cJSON_IsNumber(kind) ? (dev_kind_t)kind->valueint : DEV_OUT;
    def.pin = cJSON_IsNumber(pin) ? pin->valueint : -1;
    def.model = cJSON_IsNumber(model) ? (sensor_model_t)model->valueint : SENSOR_MODEL_NONE;
    def.addr = cJSON_IsNumber(addr) ? (uint8_t)addr->valueint : 0;
    if (def.kind == DEV_I2C_SENSOR && def.addr == 0) {
        def.addr = default_addr(def.model);
    }

    /* 校验 id/引脚冲突 */
    char id[16];
    if (def.kind == DEV_OUT) {
        snprintf(id, sizeof(id), "gpio%d", def.pin);
    } else if (def.kind == DEV_ADC) {
        snprintf(id, sizeof(id), "adc%d", def.pin);
    } else if (def.kind == DEV_DHT) {
        snprintf(id, sizeof(id), "dht%d", def.pin);
    } else {
        snprintf(id, sizeof(id), "i2c%02x", def.addr);
    }
    ESP_LOGI(TAG, "NVS 记录：name=%s kind=%d pin=%d model=%d addr=0x%02X",
             def.name, (int)def.kind, def.pin, (int)def.model, def.addr);
    if (device_find(id) != NULL) {
        ESP_LOGW(TAG, "跳过 %s：id 重复", id);
        cJSON_Delete(o);
        return;
    }
    if ((def.kind == DEV_OUT || def.kind == DEV_ADC) && device_find_pin(def.pin) != NULL) {
        ESP_LOGW(TAG, "跳过 %s：引脚 %d 已被占用", id, def.pin);
        cJSON_Delete(o);
        return;
    }
    if ((def.kind == DEV_OUT || def.kind == DEV_DHT) && !device_pin_allowed(def.pin)) {
        ESP_LOGW(TAG, "跳过 %s：数字引脚非法 %d", id, def.pin);
        cJSON_Delete(o);
        return;
    }
    if (def.kind == DEV_ADC && !device_adc_pin_allowed(def.pin)) {
        ESP_LOGW(TAG, "跳过 %s：ADC 引脚非法 %d", id, def.pin);
        cJSON_Delete(o);
        return;
    }

    device_t *d = &s_devs[s_count];
    memset(d, 0, sizeof(*d));
    snprintf(d->id, sizeof(d->id), "%s", id);
    snprintf(d->name, sizeof(d->name), "%s", def.name);
    snprintf(d->desc, sizeof(d->desc), "%s", def.desc);
    d->kind = def.kind;
    d->pin = def.pin;
    d->model = def.model;
    d->addr = def.addr;
    s_count++;

    if (d->kind == DEV_OUT) {
        gpio_reset_pin(d->pin);
        gpio_set_direction(d->pin, GPIO_MODE_INPUT_OUTPUT);
        gpio_set_level(d->pin, 0);
        d->interval_ms = 0;
    } else if (d->kind == DEV_DHT) {
        dht_attach(d);
    } else if (d->kind == DEV_ADC) {
        adc_attach(d);
    } else if (d->kind == DEV_I2C_SENSOR && s_bus != NULL) {
        if (i2c_attach(d) != ESP_OK) {
            ESP_LOGW(TAG, "%s：恢复时 I2C 挂载失败", d->id);
        }
    }
    ESP_LOGI(TAG, "恢复设备 %s（%s）kind=%d", d->id, d->name, (int)d->kind);
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

    /* 总线引脚：NVS 优先，否则默认 47/48 */
    nvs_handle_t h;
    uint8_t sda = I2C_DEFAULT_SDA, scl = I2C_DEFAULT_SCL;
    if (nvs_open(DEV_BUS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u8(h, "sda", &sda);
        nvs_get_u8(h, "scl", &scl);
        nvs_close(h);
    }
    s_bus_sda = sda;
    s_bus_scl = scl;

    adc_init();
    i2c_bus_init();

    if (nvs_open(DEV_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t n = 0;
        if (nvs_get_u8(h, "n", &n) == ESP_OK) {
            for (int i = 0; i < n && i < DEVICE_MAX; i++) {
                char key[8];
                char buf[DEV_NVS_BUF];
                size_t len = sizeof(buf);
                snprintf(key, sizeof(key), "d%d", i);
                if (nvs_get_str(h, key, buf, &len) == ESP_OK) {
                    load_device(buf);
                }
            }
        }
        nvs_close(h);
    }
    xSemaphoreGive(s_lock);

    xTaskCreatePinnedToCore(s_poll_task_fn, "dev_poll", POLL_TASK_STACK, NULL, POLL_TASK_PRIO, &s_poll_task, 1);

    ESP_LOGI(TAG, "已加载 %d 台设备（I2C %s，ADC %s）",
             s_count, s_bus ? "就绪" : "不可用", s_adc ? "就绪" : "不可用");
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 定义 / 删除 / 状态                                                   */
/* ------------------------------------------------------------------ */

static bool pin_in_use(int pin)
{
    return device_find_pin(pin) != NULL || pin == s_bus_sda || pin == s_bus_scl;
}

esp_err_t device_define(const dev_def_t *def)
{
    if (def == NULL || def->name == NULL || def->name[0] == '\0' ||
        strlen(def->name) >= DEVICE_NAME_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    if (def->desc != NULL && strlen(def->desc) >= DEVICE_DESC_MAX) {
        return ESP_ERR_INVALID_SIZE;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (def->kind != DEV_OUT && def->kind != DEV_ADC &&
        def->kind != DEV_I2C_SENSOR && def->kind != DEV_DHT) {
        return ESP_ERR_INVALID_ARG;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_count >= DEVICE_MAX) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_NO_MEM;
    }

    device_t tmp;
    memset(&tmp, 0, sizeof(tmp));
    snprintf(tmp.name, sizeof(tmp.name), "%s", def->name);
    if (def->desc != NULL) {
        snprintf(tmp.desc, sizeof(tmp.desc), "%s", def->desc);
    }
    tmp.kind = def->kind;
    tmp.pin = def->pin;
    tmp.model = def->model;
    tmp.addr = def->addr;

    if (def->kind == DEV_OUT) {
        if (!device_pin_allowed(def->pin) || pin_in_use(def->pin)) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(tmp.id, sizeof(tmp.id), "gpio%d", def->pin);
        gpio_reset_pin(def->pin);
        gpio_set_direction(def->pin, GPIO_MODE_INPUT_OUTPUT);
        gpio_set_level(def->pin, 0);
        tmp.interval_ms = 0;
    } else if (def->kind == DEV_DHT) {
        if (!device_pin_allowed(def->pin) || pin_in_use(def->pin)) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(tmp.id, sizeof(tmp.id), "dht%d", def->pin);
        dht_attach(&tmp);
        if (!dht_read(&tmp)) {
            gpio_set_level(def->pin, 1);
            gpio_reset_pin(def->pin);
            xSemaphoreGive(s_lock);
            return ESP_ERR_NOT_FOUND;
        }
    } else if (def->kind == DEV_ADC) {
        if (!device_adc_pin_allowed(def->pin) || pin_in_use(def->pin) || s_adc == NULL) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_ARG;
        }
        snprintf(tmp.id, sizeof(tmp.id), "adc%d", def->pin);
        tmp.interval_ms = 1000;
        if (adc_attach(&tmp) != ESP_OK) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_STATE;
        }
    } else {
        if (def->model == SENSOR_MODEL_NONE || def->model > SENSOR_BH1750) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_ARG;
        }
        tmp.addr = def->addr ? def->addr : default_addr(def->model);
        tmp.interval_ms = 1000;
        snprintf(tmp.id, sizeof(tmp.id), "i2c%02x", tmp.addr);
        if (device_find(tmp.id) != NULL) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_STATE;
        }
        if (s_bus == NULL) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_INVALID_STATE;
        }
        if (i2c_attach(&tmp) != ESP_OK) {
            xSemaphoreGive(s_lock);
            return ESP_ERR_NOT_FOUND;   /* 传感器无应答 */
        }
    }

    s_devs[s_count] = tmp;
    s_count++;
    esp_err_t err = persist_all();
    if (err != ESP_OK) {
        s_count--;
        memset(&s_devs[s_count], 0, sizeof(device_t));
        i2c_detach(&tmp);
        xSemaphoreGive(s_lock);
        return err;
    }
    device_t defined = s_devs[s_count - 1];
    device_defined_cb_t cb = s_define_cb;
    xSemaphoreGive(s_lock);

    ESP_LOGI(TAG, "已定义设备 %s（%s）kind=%d", defined.id, defined.name, (int)defined.kind);
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
    i2c_detach(&removed);
    if (removed.kind == DEV_OUT) {
        gpio_set_level(removed.pin, 0);
    } else if (removed.kind == DEV_DHT) {
        gpio_set_level(removed.pin, 1);
        gpio_reset_pin(removed.pin);
    }

    for (int i = idx; i < s_count - 1; i++) {
        s_devs[i] = s_devs[i + 1];
    }
    s_count--;
    memset(&s_devs[s_count], 0, sizeof(device_t));

    esp_err_t err = persist_all();
    if (err != ESP_OK) {
        s_devs[s_count++] = removed;    /* 回滚（顺序可能变化） */
        if (removed.kind == DEV_I2C_SENSOR && s_bus != NULL) {
            i2c_attach(&s_devs[s_count - 1]);
        }
    } else {
        nvs_handle_t h;
        if (nvs_open(DEV_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
            char key[8];
            snprintf(key, sizeof(key), "d%d", s_count);
            nvs_erase_key(h, key);
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
    ESP_LOGI(TAG, "已删除设备 %s", removed.id);
    if (cb != NULL) {
        cb(&removed);
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
            err = (s_devs[i].kind == DEV_OUT) ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
            break;
        }
    }
    xSemaphoreGive(s_lock);
    return err;
}

esp_err_t device_bus_get(int *sda, int *scl, bool *ready)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (sda) *sda = s_bus_sda;
    if (scl) *scl = s_bus_scl;
    if (ready) *ready = (s_bus != NULL);
    if (s_lock != NULL) {
        xSemaphoreGive(s_lock);
    }
    return ESP_OK;
}

esp_err_t device_bus_set(int sda, int scl)
{
    if (!device_pin_allowed(sda) || !device_pin_allowed(scl) || sda == scl) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_lock == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (device_find_pin(sda) != NULL || device_find_pin(scl) != NULL) {
        xSemaphoreGive(s_lock);
        return ESP_ERR_INVALID_STATE;   /* 引脚已被设备占用 */
    }
    i2c_bus_deinit();
    s_bus_sda = sda;
    s_bus_scl = scl;

    nvs_handle_t h;
    if (nvs_open(DEV_BUS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "sda", (uint8_t)sda);
        nvs_set_u8(h, "scl", (uint8_t)scl);
        nvs_commit(h);
        nvs_close(h);
    }

    esp_err_t err = i2c_bus_init();
    xSemaphoreGive(s_lock);
    return err;
}

int device_bus_scan(uint8_t *addrs, int max)
{
    if (addrs == NULL || max <= 0) {
        return 0;
    }
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
    }
    if (s_bus == NULL) {
        if (s_lock != NULL) xSemaphoreGive(s_lock);
        return -1;
    }
    int n = 0;
    for (uint8_t addr = 0x08; addr <= 0x77 && n < max; addr++) {
        if (i2c_master_probe(s_bus, addr, 20) == ESP_OK) {
            addrs[n++] = addr;
        }
    }
    if (s_lock != NULL) xSemaphoreGive(s_lock);
    return n;
}

/* ------------------------------------------------------------------ */
/* 轮询任务                                                            */
/* ------------------------------------------------------------------ */

static void s_poll_task_fn(void *arg)
{
    (void)arg;
    static device_t updates[DEVICE_MAX];   /* 设备结构较大，不放任务栈 */
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(POLL_PERIOD_MS));
        int n = 0;
        xSemaphoreTake(s_lock, portMAX_DELAY);
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < s_count; i++) {
            device_t *d = &s_devs[i];
            if (d->interval_ms > 0 &&
                now - d->last_try_us < (int64_t)d->interval_ms * 1000) {
                continue;
            }
            bool ok = false;
            d->last_try_us = now;
            if (d->kind == DEV_ADC) {
                ok = adc_read(d);
            } else if (d->kind == DEV_DHT) {
                ok = dht_read(d);
            } else if (d->kind == DEV_I2C_SENSOR) {
                ok = i2c_read(d);
            }
            if (ok) {
                updates[n++] = *d;
            }
        }
        device_value_cb_t cb = s_value_cb;
        xSemaphoreGive(s_lock);

        if (cb != NULL) {
            for (int i = 0; i < n; i++) {
                cb(&updates[i]);
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* 监听器                                                              */
/* ------------------------------------------------------------------ */

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

void device_registry_set_value_listener(device_value_cb_t cb)
{
    if (s_lock != NULL) {
        xSemaphoreTake(s_lock, portMAX_DELAY);
        s_value_cb = cb;
        xSemaphoreGive(s_lock);
    } else {
        s_value_cb = cb;
    }
}
