/**
 * @file web_console.c
 * @brief 本地 Web 控制台实现（esp_http_server）
 *
 * 端点：
 *   GET  /                      状态页（内嵌 HTML）
 *   GET  /api/status            状态 JSON
 *   GET  /api/gpio              已定义设备 / 可定义引脚 / 板载占用引脚
 *   POST /api/gpio/define       定义命名输出（JSON: pin/name/desc）
 *   POST /api/gpio/remove       删除命名输出（JSON: id）
 *   POST /api/device?id=&value=0|1 设置命名设备开关
 *   GET  /api/logs              列出 TF 日志
 *   GET  /api/logs/<file>       下载 TF 日志（防目录穿越）
 *
 * 安全：设备引脚由 device_registry 白名单校验；日志下载校验文件名。
 */
#include "web_console.h"
#include "www_index.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>

#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_http_server.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "cJSON.h"
#include "esp_heap_caps.h"

#include "net_manager.h"
#include "ha_mqtt.h"
#include "tf_storage.h"
#include "app_cfg.h"
#include "sensor_history.h"
#include "device_registry.h"

#if CONFIG_WEB_CONSOLE_MDNS_ENABLE
#include "mdns.h"
#endif

static const char *TAG = "web_console";

#define URI_LOGS_PREFIX  "/api/logs/"

static httpd_handle_t s_server = NULL;

/* ------------------------------------------------------------------ */
/* 工具：查询参数                                                      */
/* ------------------------------------------------------------------ */

static int query_int(httpd_req_t *req, const char *key, int def)
{
    char qs[160];
    char v[24] = { 0 };
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) {
        return def;
    }
    if (httpd_query_key_value(qs, key, v, sizeof(v)) != ESP_OK) {
        return def;
    }
    return atoi(v);
}

static esp_err_t send_json(httpd_req_t *req, cJSON *obj)
{
    char *s = cJSON_PrintUnformatted(obj);
    cJSON_Delete(obj);
    if (s == NULL) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "oom");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t r = httpd_resp_sendstr(req, s);
    free(s);
    return r;
}

static esp_err_t send_err_json(httpd_req_t *req, const char *code, const char *msg)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", false);
    cJSON_AddStringToObject(o, "error", code);
    cJSON_AddStringToObject(o, "message", msg);
    return send_json(req, o);
}

/* ------------------------------------------------------------------ */
/* 处理器                                                              */
/* ------------------------------------------------------------------ */

static esp_err_t h_root(httpd_req_t *req)
{
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    return httpd_resp_sendstr(req, WEB_CONSOLE_INDEX_HTML);
}

static esp_err_t h_status(httpd_req_t *req)
{
    ha_mqtt_set_http_stage("status");
    net_manager_status_t st = net_manager_get_status();
    ha_sensor_t sn;
    ha_mqtt_get_last_sensor(&sn);
    bool stale = (sn.rx_us == 0) ||
                 ((esp_timer_get_time() - sn.rx_us) >
                  (int64_t)CONFIG_HA_MQTT_SENSOR_STALE_MS * 1000);

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "uplink", net_manager_backend_name(st.active));
    cJSON_AddStringToObject(o, "ip", st.active_ip[0] ? st.active_ip : "-");
    cJSON_AddBoolToObject(o, "eth_ready", st.eth_ready);
    cJSON_AddBoolToObject(o, "wifi_ready", st.wifi_ready);
    cJSON_AddNumberToObject(o, "uptime", (double)(esp_timer_get_time() / 1000000));
    cJSON_AddBoolToObject(o, "mqtt", ha_mqtt_is_connected());
    cJSON_AddBoolToObject(o, "tf_ready", tf_storage_is_ready());
    cJSON_AddNumberToObject(o, "tf_written", (double)tf_storage_written_count());
    cJSON_AddBoolToObject(o, "sensor_valid", sn.rx_us != 0);
    cJSON_AddBoolToObject(o, "sensor_stale", stale);
    cJSON_AddNumberToObject(o, "sensor_temp", sn.temp);
    cJSON_AddNumberToObject(o, "sensor_hum", sn.hum);
    cJSON_AddNumberToObject(o, "sensor_rssi", sn.rssi);
    /* 内存诊断（排查崩溃/泄漏） */
    cJSON_AddNumberToObject(o, "heap_free", (double)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "heap_min", (double)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "heap_block", (double)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
    cJSON_AddNumberToObject(o, "psram_free", (double)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
#if CONFIG_WEB_CONSOLE_MDNS_ENABLE
    cJSON_AddStringToObject(o, "mdns", CONFIG_WEB_CONSOLE_MDNS_HOSTNAME);
#endif
    return send_json(req, o);
}

/** GET /api/gpio —— 已定义设备 / 可定义引脚 / 板载占用引脚 */
static esp_err_t h_gpio_info(httpd_req_t *req)
{
    ha_mqtt_set_http_stage("gpio");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);

    cJSON *def = cJSON_AddArrayToObject(o, "defined");
    for (int i = 0; i < device_count(); i++) {
        const device_t *d = device_at(i);
        if (d == NULL) {
            continue;
        }
        bool on = false;
        device_get(d->id, &on);
        cJSON *it = cJSON_CreateObject();
        cJSON_AddStringToObject(it, "id", d->id);
        cJSON_AddStringToObject(it, "name", d->name);
        cJSON_AddStringToObject(it, "desc", d->desc);
        cJSON_AddNumberToObject(it, "pin", d->pin);
        cJSON_AddBoolToObject(it, "on", on);
        cJSON_AddItemToArray(def, it);
    }

    cJSON *avail = cJSON_AddArrayToObject(o, "available");
    for (int pin = 0; pin <= 48; pin++) {
        if (device_pin_allowed(pin) && device_find_pin(pin) == NULL) {
            cJSON *n = cJSON_CreateNumber(pin);
            if (n != NULL) {
                cJSON_AddItemToArray(avail, n);
            }
        }
    }

    cJSON *res = cJSON_AddArrayToObject(o, "reserved");
    for (int i = 0; i < device_reserved_count(); i++) {
        const device_reserved_t *r = device_reserved_at(i);
        if (r == NULL) {
            continue;
        }
        cJSON *it = cJSON_CreateObject();
        cJSON_AddNumberToObject(it, "pin", r->pin);
        cJSON_AddStringToObject(it, "reason", r->reason);
        cJSON_AddItemToArray(res, it);
    }
    return send_json(req, o);
}

/** 读取完整请求体到 buf（含结尾 NUL）；失败返回 -1 */
static int recv_body(httpd_req_t *req, char *buf, size_t cap)
{
    int total = req->content_len;
    if (total <= 0 || total >= (int)cap) {
        return -1;
    }
    int got = 0;
    while (got < total) {
        int n = httpd_req_recv(req, buf + got, total - got);
        if (n <= 0) {
            return -1;
        }
        got += n;
    }
    buf[got] = '\0';
    return got;
}

/** POST /api/gpio/define —— 定义一个命名输出（JSON: {"pin","name","desc"}） */
static esp_err_t h_gpio_define(httpd_req_t *req)
{
    ha_mqtt_set_http_stage("define");
    char body[512];
    if (recv_body(req, body, sizeof(body)) < 0) {
        return send_err_json(req, "bad_body", "请求体为空或过大");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return send_err_json(req, "bad_json", "JSON 解析失败");
    }
    cJSON *pin = cJSON_GetObjectItem(root, "pin");
    cJSON *name = cJSON_GetObjectItem(root, "name");
    cJSON *desc = cJSON_GetObjectItem(root, "desc");
    if (!cJSON_IsNumber(pin) || !cJSON_IsString(name) || name->valuestring[0] == '\0') {
        cJSON_Delete(root);
        return send_err_json(req, "bad_args", "需要 pin 与 name");
    }
    if (strlen(name->valuestring) >= DEVICE_NAME_MAX) {
        cJSON_Delete(root);
        return send_err_json(req, "bad_name", "设备名过长");
    }
    if (cJSON_IsString(desc) && strlen(desc->valuestring) >= DEVICE_DESC_MAX) {
        cJSON_Delete(root);
        return send_err_json(req, "bad_desc", "说明过长");
    }
    int pin_no = pin->valueint;
    esp_err_t err = device_define(name->valuestring, pin_no,
                                  cJSON_IsString(desc) ? desc->valuestring : "");
    cJSON_Delete(root);

    if (err == ESP_ERR_INVALID_ARG) {
        return send_err_json(req, "bad_pin", "引脚不可用或名称非法");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_err_json(req, "pin_used", "该引脚已被定义");
    }
    if (err == ESP_ERR_INVALID_SIZE) {
        return send_err_json(req, "bad_desc", "说明过长");
    }
    if (err == ESP_ERR_NO_MEM) {
        return send_err_json(req, "full", "设备数量已达上限");
    }
    if (err != ESP_OK) {
        return send_err_json(req, "define_failed", esp_err_to_name(err));
    }

    const device_t *d = device_find_pin(pin_no);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    if (d != NULL) {
        cJSON_AddStringToObject(o, "id", d->id);
        cJSON_AddStringToObject(o, "name", d->name);
        cJSON_AddStringToObject(o, "desc", d->desc);
        cJSON_AddNumberToObject(o, "pin", d->pin);
        cJSON_AddBoolToObject(o, "on", d->on);
    }
    return send_json(req, o);
}

/** POST /api/gpio/remove —— 删除一个已定义输出（JSON: {"id"}） */
static esp_err_t h_gpio_remove(httpd_req_t *req)
{
    ha_mqtt_set_http_stage("remove");
    char body[128];
    if (recv_body(req, body, sizeof(body)) < 0) {
        return send_err_json(req, "bad_body", "请求体为空或过大");
    }

    cJSON *root = cJSON_Parse(body);
    if (root == NULL) {
        return send_err_json(req, "bad_json", "JSON 解析失败");
    }
    cJSON *id = cJSON_GetObjectItem(root, "id");
    if (!cJSON_IsString(id) || id->valuestring[0] == '\0' ||
        strlen(id->valuestring) >= sizeof(((device_t *)0)->id)) {
        cJSON_Delete(root);
        return send_err_json(req, "bad_id", "缺少或非法的 id");
    }
    char idbuf[16];
    snprintf(idbuf, sizeof(idbuf), "%s", id->valuestring);
    cJSON_Delete(root);

    esp_err_t err = device_remove(idbuf);
    if (err == ESP_ERR_NOT_FOUND) {
        return send_err_json(req, "unknown_device", "设备未定义");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return send_err_json(req, "not_ready", "设备未初始化");
    }
    if (err != ESP_OK) {
        return send_err_json(req, "remove_failed", esp_err_to_name(err));
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "id", idbuf);
    return send_json(req, o);
}

/** POST /api/device?id=<id>&value=0|1 —— 设置命名设备开关 */
static esp_err_t h_device_set(httpd_req_t *req)
{
    ha_mqtt_set_http_stage("device");
    char qs[160];
    char id[32] = { 0 };
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) {
        return send_err_json(req, "bad_query", "缺少参数");
    }
    if (httpd_query_key_value(qs, "id", id, sizeof(id)) != ESP_OK) {
        return send_err_json(req, "bad_id", "缺少 id 参数");
    }
    int val = query_int(req, "value", -1);
    if (val != 0 && val != 1) {
        return send_err_json(req, "bad_value", "value 必须为 0 或 1");
    }

    const device_t *d = device_find(id);
    if (d == NULL) {
        return send_err_json(req, "unknown_device", "设备未定义");
    }

    esp_err_t err = device_set(id, val != 0);
    if (err == ESP_ERR_INVALID_STATE) {
        return send_err_json(req, "not_ready", "设备未初始化");
    }
    if (err == ESP_ERR_NOT_SUPPORTED) {
        return send_err_json(req, "read_only", "该设备不支持写入");
    }
    if (err != ESP_OK) {
        return send_err_json(req, "set_failed", esp_err_to_name(err));
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "id", id);
    cJSON_AddStringToObject(o, "name", d->name);
    cJSON_AddNumberToObject(o, "pin", d->pin);
    cJSON_AddBoolToObject(o, "on", val != 0);
    return send_json(req, o);
}

/** GET /api/logs —— 列出 TF 日志目录 */
static esp_err_t h_logs_list(httpd_req_t *req)
{
    ha_mqtt_set_http_stage("logs");
    cJSON *o = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(o, "files");
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddBoolToObject(o, "ready", tf_storage_is_ready());

    if (tf_storage_is_ready()) {
        const char *dir = tf_storage_log_dir();
        DIR *d = opendir(dir);
        if (d != NULL) {
            struct dirent *e;
            while ((e = readdir(d)) != NULL) {
                if (e->d_name[0] == '.') {
                    continue;
                }
                char path[320];
                struct stat stt;
                snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
                if (stat(path, &stt) != 0 || !S_ISREG(stt.st_mode)) {
                    continue;
                }
                cJSON *f = cJSON_CreateObject();
                cJSON_AddStringToObject(f, "name", e->d_name);
                cJSON_AddNumberToObject(f, "size", (double)stt.st_size);
                cJSON_AddItemToArray(arr, f);
            }
            closedir(d);
        }
    }
    return send_json(req, o);
}

/** GET /api/logs/<file> —— 下载（防目录穿越） */
static esp_err_t h_logs_get(httpd_req_t *req)
{
    const char *name = req->uri + strlen(URI_LOGS_PREFIX);
    if (*name == '\0' || strstr(name, "..") != NULL || strchr(name, '/') != NULL) {
        return send_err_json(req, "bad_name", "非法文件名");
    }
    if (!tf_storage_is_ready()) {
        return send_err_json(req, "no_tf", "TF 卡未挂载");
    }
    char path[320];
    snprintf(path, sizeof(path), "%s/%s", tf_storage_log_dir(), name);

    FILE *f = fopen(path, "r");
    if (f == NULL) {
        httpd_resp_set_status(req, "404 Not Found");
        return httpd_resp_sendstr(req, "404: 文件不存在");
    }
    httpd_resp_set_type(req, "text/csv; charset=utf-8");
    char buf[512];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (httpd_resp_send_chunk(req, buf, n) != ESP_OK) {
            break;
        }
    }
    fclose(f);
    return httpd_resp_send_chunk(req, NULL, 0);
}

/* ------------------------------------------------------------------ */
/* 参数配置 / OTA 升级                                                 */
/* ------------------------------------------------------------------ */
/** 简单 URL 解码（%XX 与 '+'） */
static void url_decode(char *s)
{
    char *o = s;
    while (*s) {
        if (*s == '%' && s[1] && s[2]) {
            char hex[3] = { s[1], s[2], 0 };
            *o++ = (char)strtol(hex, NULL, 16);
            s += 3;
        } else if (*s == '+') {
            *o++ = ' ';
            s++;
        } else {
            *o++ = *s++;
        }
    }
    *o = '\0';
}

/** GET /api/config —— 当前配置（不回传密码明文） */
static esp_err_t h_config_get(httpd_req_t *req)
{
    ha_mqtt_set_http_stage("config_get");
    const app_cfg_t *c = app_cfg_get();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddStringToObject(o, "wifi_ssid", c->wifi_ssid);
    cJSON_AddBoolToObject(o, "wifi_pass_set", c->wifi_pass[0] != '\0');
    cJSON_AddStringToObject(o, "mqtt_host", c->mqtt_host);
    cJSON_AddNumberToObject(o, "mqtt_port", c->mqtt_port);
    cJSON_AddStringToObject(o, "topic_notify", c->topic_notify);
    cJSON_AddStringToObject(o, "topic_sensor", c->topic_sensor);
    cJSON_AddBoolToObject(o, "from_nvs", app_cfg_from_nvs());
    return send_json(req, o);
}

/** POST /api/config?wifi_ssid=..&mqtt_port=..&action=reset —— 写入 NVS，重启生效 */
static esp_err_t h_config_post(httpd_req_t *req)
{
    ha_mqtt_set_http_stage("config_post");
    char qs[512];
    if (httpd_req_get_url_query_str(req, qs, sizeof(qs)) != ESP_OK) {
        return send_err_json(req, "bad_query", "缺少参数");
    }
    char act[16] = { 0 };
    if (httpd_query_key_value(qs, "action", act, sizeof(act)) == ESP_OK &&
        strcmp(act, "reset") == 0) {
        esp_err_t e = app_cfg_reset();
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", e == ESP_OK);
        cJSON_AddBoolToObject(o, "reboot_required", true);
        return send_json(req, o);
    }

    static const char *str_keys[] = { "wifi_ssid", "wifi_pass", "mqtt_host",
                                      "topic_notify", "topic_sensor" };
    int applied = 0;
    char val[96];
    for (size_t i = 0; i < sizeof(str_keys) / sizeof(str_keys[0]); i++) {
        if (httpd_query_key_value(qs, str_keys[i], val, sizeof(val)) == ESP_OK) {
            url_decode(val);
            if (app_cfg_set(str_keys[i], val) == ESP_OK) {
                applied++;
            }
        }
    }
    char port[8] = { 0 };
    if (httpd_query_key_value(qs, "mqtt_port", port, sizeof(port)) == ESP_OK) {
        if (app_cfg_set_int("mqtt_port", atoi(port)) == ESP_OK) {
            applied++;
        }
    }

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", applied > 0);
    cJSON_AddNumberToObject(o, "applied", applied);
    cJSON_AddBoolToObject(o, "reboot_required", true);
    return send_json(req, o);
}

/** POST /api/ota —— 上传固件（原始 bin），成功后重启 */
static esp_err_t h_ota_post(httpd_req_t *req)
{
    ha_mqtt_set_http_stage("ota");
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (part == NULL) {
        return send_err_json(req, "no_part", "无可用 OTA 分区");
    }
    esp_ota_handle_t handle;
    if (esp_ota_begin(part, OTA_SIZE_UNKNOWN, &handle) != ESP_OK) {
        return send_err_json(req, "ota_begin", "OTA 初始化失败");
    }

    char buf[1024];
    int  remaining = req->content_len;
    int  total = 0;
    ESP_LOGI(TAG, "OTA 开始：%d 字节 -> %s", remaining, part->label);
    while (remaining > 0) {
        int n = httpd_req_recv(req, buf, remaining > (int)sizeof(buf) ? sizeof(buf) : remaining);
        if (n <= 0) {
            esp_ota_abort(handle);
            return send_err_json(req, "recv", "接收失败");
        }
        if (esp_ota_write(handle, buf, n) != ESP_OK) {
            esp_ota_abort(handle);
            return send_err_json(req, "write", "写入失败");
        }
        total += n;
        remaining -= n;
    }
    if (esp_ota_end(handle) != ESP_OK || esp_ota_set_boot_partition(part) != ESP_OK) {
        return send_err_json(req, "finish", "OTA 完成失败（固件校验不通过？）");
    }
    ESP_LOGI(TAG, "OTA 成功：%d 字节，重启生效", total);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

/** GET /api/history —— 温湿度历史（供页面曲线图） */
static esp_err_t h_history(httpd_req_t *req)
{
    ha_mqtt_set_http_stage("history");
    const int MAXN = 240;
    sensor_sample_t *buf = malloc(sizeof(sensor_sample_t) * MAXN);
    if (buf == NULL) {
        return send_err_json(req, "oom", "内存不足");
    }
    int n = sensor_history_get(buf, MAXN);

    time_t  now_wall = time(NULL);
    int64_t now_us   = esp_timer_get_time();
    bool    wall_ok  = now_wall > 1700000000;   /* 已校时 */

    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", true);
    cJSON_AddNumberToObject(o, "count", n);
    cJSON_AddBoolToObject(o, "wall_clock", wall_ok);
    cJSON *arr = cJSON_AddArrayToObject(o, "items");
    for (int i = 0; i < n; i++) {
        cJSON *it = cJSON_CreateObject();
        double t = wall_ok ? (double)(now_wall - (now_us - buf[i].t_us) / 1000000)
                           : (double)(buf[i].t_us / 1000000);
        cJSON_AddNumberToObject(it, "t", t);
        cJSON_AddNumberToObject(it, "temp", buf[i].temp);
        cJSON_AddNumberToObject(it, "hum", buf[i].hum);
        cJSON_AddItemToArray(arr, it);
    }
    free(buf);
    return send_json(req, o);
}

/** POST /api/reboot —— 保存后重启设备 */
static esp_err_t h_reboot_post(httpd_req_t *req)
{
    ha_mqtt_set_http_stage("reboot");
    ESP_LOGI(TAG, "收到重启请求，2 秒后重启");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"rebooting\":true}");
    vTaskDelay(pdMS_TO_TICKS(2000));
    esp_restart();
    return ESP_OK;
}

/* ------------------------------------------------------------------ */
/* 初始化                                                              */
/* ------------------------------------------------------------------ */

static esp_err_t start_mdns(void)
{
#if CONFIG_WEB_CONSOLE_MDNS_ENABLE
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mDNS 初始化失败：%s（仍可用 IP 访问）", esp_err_to_name(err));
        return err;
    }
    mdns_hostname_set(CONFIG_WEB_CONSOLE_MDNS_HOSTNAME);
    mdns_instance_name_set("ESP32-S3 双网网关");
    mdns_service_add(NULL, "_http", "_tcp", CONFIG_WEB_CONSOLE_PORT, NULL, 0);
    ESP_LOGI(TAG, "mDNS 已启动：http://%s.local/", CONFIG_WEB_CONSOLE_MDNS_HOSTNAME);
#endif
    return ESP_OK;
}

esp_err_t web_console_init(void)
{
    if (s_server != NULL) {
        return ESP_OK;
    }

    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port      = CONFIG_WEB_CONSOLE_PORT;
    cfg.max_uri_handlers = 16;
    cfg.stack_size       = 12288;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn     = httpd_uri_match_wildcard;   /* 支持通配符路径（日志下载） */

    ESP_RETURN_ON_ERROR(httpd_start(&s_server, &cfg), TAG, "HTTP 服务启动失败");

    static const httpd_uri_t uris[] = {
        { .uri = "/",                .method = HTTP_GET,  .handler = h_root },
        { .uri = "/api/status",      .method = HTTP_GET,  .handler = h_status },
        { .uri = "/api/gpio",        .method = HTTP_GET,  .handler = h_gpio_info },
        { .uri = "/api/gpio/define", .method = HTTP_POST, .handler = h_gpio_define },
        { .uri = "/api/gpio/remove", .method = HTTP_POST, .handler = h_gpio_remove },
        { .uri = "/api/device",      .method = HTTP_POST, .handler = h_device_set },
        { .uri = "/api/logs",        .method = HTTP_GET,  .handler = h_logs_list },
        { .uri = "/api/logs/*",      .method = HTTP_GET,  .handler = h_logs_get },
        { .uri = "/api/config",      .method = HTTP_GET,  .handler = h_config_get },
        { .uri = "/api/config",      .method = HTTP_POST, .handler = h_config_post },
        { .uri = "/api/history",     .method = HTTP_GET,  .handler = h_history },
        { .uri = "/api/reboot",      .method = HTTP_POST, .handler = h_reboot_post },
        { .uri = "/api/ota",         .method = HTTP_POST, .handler = h_ota_post },
    };
    for (size_t i = 0; i < sizeof(uris) / sizeof(uris[0]); i++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_server, &uris[i]));
    }

    start_mdns();

    ESP_LOGI(TAG, "Web 控制台已启动：端口 %d（无 Token 鉴权）",
             CONFIG_WEB_CONSOLE_PORT);
    return ESP_OK;
}