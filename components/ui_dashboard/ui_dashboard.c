/**
 * @file ui_dashboard.c
 * @brief LVGL 本地看板：ST7735S 128x160 显示双网状态（上行链路 / IP / 可用性 / 运行时长）
 *
 * 数据来源：net_manager（net_manager_get_status() 快照 + 监听器回调即时刷新）
 *
 * 线程模型（重要）：
 *  - LVGL 由 esp_lvgl_port 自建任务驱动；所有 UI 操作必须持 lvgl_port_lock()。
 *  - 周期刷新任务：先取网络快照，再加锁更新 UI；
 *    【绝不持 LVGL 锁去调用 net_manager_get_status()】，否则与 net_manager
 *    持自身互斥锁回调监听器构成锁序死锁（ABBA）。
 *  - net_manager 监听器回调：持 net_manager 锁期间仅做“快速刷 UI”，
 *    配合刷新任务的取锁顺序，可保证无死锁。
 *
 * 说明：屏显使用拉丁字符（LVGL 内置 Montserrat 字体不含中文字形）；
 *       如需中文界面，后续可嵌入中文字体（如 lv_font_simsun_16_cjk）。
 *
 * HA 通知：接收 ha_mqtt 投递的 notify（其投递任务上下文，非 MQTT 回调），
 * 在通知显示窗口（CONFIG_HA_MQTT_NOTIFY_SHOW_MS）内以 banner 覆盖 KPI 区，
 * 超时自动恢复常规状态显示。
 */
#include "ui_dashboard.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "driver/gpio.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "device_registry.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "sdkconfig.h"    /* CONFIG_LCD_ST7735_ROTATION_*：使看板方向与面板驱动一致 */
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "lvgl.h"
#include "esp_lvgl_port.h"
#include "esp_lcd_panel_io.h"
#include "lcd_st7735.h"

/* 自建中文字体（GB2312 全集 + ASCII，14px bpp4，由 simhei.ttf 生成）。
   生成命令见 README §6.9；LVGL 内置 CJK 字体字符集不全（缺“门/铃/按/请/灯”等），
   故改用本字体。 */
LV_FONT_DECLARE(lv_font_cjk_14);
#include "net_manager.h"
#include "ha_mqtt.h"
#include "board_pins.h"

static const char *TAG = "ui_dashboard";

#define UI_W            BOARD_LCD_H_RES   /*!< 128 */
#define UI_H            BOARD_LCD_V_RES   /*!< 160 */
#define UI_REFRESH_MS   1000              /*!< 运行时长/状态周期刷新 */
#define UI_TASK_STACK   4096
#define UI_TASK_PRIO    6
#define UI_LVGL_LOCK_TIMEOUT_MS  20              /*!< 避免 LVGL 锁异常时阻塞业务任务 */

static TaskHandle_t s_task = NULL;                                      /*!< 刷新任务 */
static uint32_t s_ui_w = UI_W;      /*!< 旋转后的显示宽（默认 128） */
static uint32_t s_ui_h = UI_H;      /*!< 旋转后的显示高（默认 160） */

/* HA 通知 banner */
static lv_obj_t *s_notify_panel = NULL;   /*!< 通知面板（默认隐藏，覆盖 KPI 区） */
static lv_obj_t *s_notify_title = NULL;   /*!< 通知标题（level 色） */
static lv_obj_t *s_notify_msg   = NULL;   /*!< 通知正文（多行 wrap） */
static bool       s_notify_active = false;/*!< 当前是否处于通知显示期 */
static uint64_t   s_notify_until_us = 0;  /*!< 通知恢复截止时刻（esp_timer） */

/* LVGL 控件句柄 */
static lv_obj_t *s_content = NULL;      /*!< 常规内容容器（通知横幅出现时整体隐藏） */
static lv_obj_t *s_lbl_clock = NULL;    /*!< 顶栏右侧时间/链路 */
static lv_obj_t *s_lbl_t_val = NULL;    /*!< 温度数值 */
static lv_obj_t *s_lbl_h_val = NULL;    /*!< 湿度数值 */
static lv_obj_t *s_sw_row[3] = { 0 };   /*!< 开关行 */
static lv_obj_t *s_sw_led[3] = { 0 };   /*!< 开关状态灯 */
static lv_obj_t *s_sw_name[3] = { 0 };
static lv_obj_t *s_sw_state[3] = { 0 };
static char       s_sw_id[3][16];       /*!< 当前显示的开关 id */
static int        s_sw_shown = 0;
static lv_obj_t *s_info_lbl[5] = { 0 }; /*!< 底部信息行 */
static int        s_info_kind[5];       /*!< 0=IP 1=上行 2=链路 3=运行 4=日期 */
static int        s_info_n = 0;

/* 二维码页 */
static lv_obj_t *s_qr_content = NULL;
static lv_obj_t *s_qr_obj = NULL;
static lv_obj_t *s_qr_label = NULL;
static char       s_qr_url[64];
static bool       s_show_qr = false;
static int64_t    s_page_switch_us = 0;
static int64_t    s_qr_paused_until_us = 0;

#define BOOT_GPIO_NUM       0
#define BOOT_LONG_PRESS_MS  2000
#define BOOT_POLL_MS        50
#define BOOT_TASK_STACK     2048
#define BOOT_TASK_PRIO      3

static ha_notify_t s_last_notify;       /*!< 最近通知（重建时恢复 banner） */
static ha_sensor_t s_sensor;            /*!< MQTT 传感器数据（无自定义传感器时兜底） */
static bool s_sensor_has   = false;
static lv_display_t *s_disp = NULL;     /*!< LVGL 显示设备（供截图使用） */

static uint64_t s_boot_us = 0;          /*!< 启动时刻（esp_timer 单调时钟） */

static bool     s_need_rebuild = true;
static uint32_t s_cache_mask = 0xFFFFFFFF;
static int      s_cache_n = 0;
static char     s_cache_id[8][16];
static char     s_cache_name[8][DEVICE_NAME_MAX];

#define UI_ITEMS_NS   "uiscreen"
#define UI_BODY_LINES 8
static uint32_t s_items = UI_SCREEN_TEMP_HUM | UI_SCREEN_IP |
                          UI_SCREEN_UPLINK | UI_SCREEN_LINK | UI_SCREEN_UPTIME;

/* ---------------------------------------------------------------------------
 * 内容拼装（128x160，CJK 字体；最多 8 行）
 * ------------------------------------------------------------------------- */

static void body_append(char *out, size_t n, size_t *used, int *lines, const char *fmt, ...)
{
    if (*lines >= UI_BODY_LINES) {
        return;
    }
    int prefix = (*lines > 0) ? snprintf(out + *used, n - *used, "\n") : 0;
    *used += (size_t)prefix;
    va_list ap;
    va_start(ap, fmt);
    int k = vsnprintf(out + *used, n - *used, fmt, ap);
    va_end(ap);
    if (k > 0) {
        *used += (size_t)k;
        if (*used >= n) {
            *used = n - 1;
        }
    }
    (*lines)++;
}

/** 把 UTF-8 字符串截断到不超过 max 字节（不切断多字节字符） */
static void utf8_copy(char *dst, size_t dst_size, const char *src, size_t max_bytes)
{
    size_t limit = max_bytes < dst_size - 1 ? max_bytes : dst_size - 1;
    size_t len = 0;
    while (src[len] != '\0' && len < limit) {
        unsigned char c = (unsigned char)src[len];
        size_t clen = (c < 0x80) ? 1 : (c < 0xE0) ? 2 : (c < 0xF0) ? 3 : 4;
        if (len + clen > limit) {
            break;
        }
        len += clen;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

/** 取第一个有效的温湿度传感器；没有则用 MQTT 节点数据 */
static bool latest_temp_hum(float *t, float *h)
{
    for (int i = 0; i < device_count(); i++) {
        const device_t *d = device_at(i);
        if (d == NULL || device_metric_count(d) < 2) {
            continue;
        }
        if (strcmp(device_metric_key(d, 0), "temp") == 0 &&
            d->valid[0] && d->valid[1]) {
            *t = d->value[0];
            *h = d->value[1];
            return true;
        }
    }
    if (s_sensor_has &&
        (esp_timer_get_time() - s_sensor.rx_us) <=
            (int64_t)CONFIG_HA_MQTT_SENSOR_STALE_MS * 1000) {
        *t = s_sensor.temp;
        *h = s_sensor.hum;
        return true;
    }
    return false;
}

static int fixed_line_count(uint32_t items)
{
    int n = 0;
    n += (items & UI_SCREEN_TEMP_HUM) ? 1 : 0;
    n += (items & UI_SCREEN_IP) ? 1 : 0;
    n += (items & UI_SCREEN_DATETIME) ? 1 : 0;
    n += (items & UI_SCREEN_UPLINK) ? 1 : 0;
    n += (items & UI_SCREEN_LINK) ? 1 : 0;
    n += (items & UI_SCREEN_UPTIME) ? 1 : 0;
    return n;
}

static void compose_body(char *out, size_t n, const net_manager_status_t *st)
{
    out[0] = '\0';
    size_t used = 0;
    int lines = 0;
    const uint32_t items = s_items;

    if (items & UI_SCREEN_TEMP_HUM) {
        float t, h;
        if (latest_temp_hum(&t, &h)) {
            body_append(out, n, &used, &lines, "温%.1f℃ 湿%.1f%%", t, h);
        } else {
            body_append(out, n, &used, &lines, "温-- 湿--");
        }
    }

    if (items & UI_SCREEN_SWITCHES) {
        int slots = UI_BODY_LINES - fixed_line_count(items);
        if (slots > 5) {
            slots = 5;
        }
        int total = 0;
        for (int i = 0; i < device_count(); i++) {
            const device_t *d = device_at(i);
            if (d != NULL && d->kind == DEV_OUT) {
                total++;
            }
        }
        int shown = 0;
        for (int i = 0; i < device_count() && shown < slots; i++) {
            const device_t *d = device_at(i);
            if (d == NULL || d->kind != DEV_OUT) {
                continue;
            }
            char name[20];
            utf8_copy(name, sizeof(name), d->name, 18);
            body_append(out, n, &used, &lines, "%s %s", name, d->on ? "开" : "关");
            shown++;
        }
        if (total > shown && slots > 0) {
            body_append(out, n, &used, &lines, "… +%d", total - shown);
        }
    }

    if (items & UI_SCREEN_IP) {
        body_append(out, n, &used, &lines, "IP %s",
                    st->active_ip[0] ? st->active_ip : "-");
    }

    if (items & UI_SCREEN_DATETIME) {
        time_t now = time(NULL);
        struct tm tm;
        if (now > 1700000000 && localtime_r(&now, &tm) != NULL) {
            body_append(out, n, &used, &lines, "%04d-%02d-%02d %02d:%02d",
                        tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
                        tm.tm_hour, tm.tm_min);
        } else {
            body_append(out, n, &used, &lines, "时间 --:--");
        }
    }

    if (items & UI_SCREEN_UPLINK) {
        body_append(out, n, &used, &lines, "上行 %s",
                    net_manager_backend_name(st->active));
    }

    if (items & UI_SCREEN_LINK) {
        body_append(out, n, &used, &lines, "ETH %s WiFi %s",
                    st->eth_ready ? "OK" : "--",
                    st->wifi_ready ? "OK" : "--");
    }

    if (items & UI_SCREEN_UPTIME) {
        uint64_t sec = (esp_timer_get_time() - s_boot_us) / 1000000ULL;
        body_append(out, n, &used, &lines, "运行 %02llu:%02llu:%02llu",
                    sec / 3600, (sec % 3600) / 60, sec % 60);
    }
}

/* ---------------------------------------------------------------------------
 * 结构化布局：顶栏 / 温湿度卡片 / 开关列表 / 信息行
 * （128x160；所有函数须在持 LVGL 锁时调用）
 * ------------------------------------------------------------------------- */

static lv_obj_t *ui_panel(lv_obj_t *parent, int x, int y, int w, int h,
                          uint32_t bg, int radius)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_set_pos(o, x, y);
    lv_obj_set_style_bg_color(o, lv_color_hex(bg), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(o, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_width(o, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(o, radius, LV_PART_MAIN);
    lv_obj_set_style_pad_all(o, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    return o;
}

static lv_obj_t *ui_label(lv_obj_t *parent, int x, int y, const lv_font_t *font,
                          uint32_t color, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_pos(l, x, y);
    lv_obj_set_style_text_font(l, font, LV_PART_MAIN);
    lv_obj_set_style_text_color(l, lv_color_hex(color), LV_PART_MAIN);
    lv_label_set_text(l, text);
    return l;
}

static int collect_out_cache(void)
{
    int n = 0;
    for (int i = 0; i < device_count() && n < 8; i++) {
        const device_t *d = device_at(i);
        if (d == NULL || d->kind != DEV_OUT) {
            continue;
        }
        snprintf(s_cache_id[n], sizeof(s_cache_id[0]), "%s", d->id);
        snprintf(s_cache_name[n], sizeof(s_cache_name[0]), "%s", d->name);
        n++;
    }
    s_cache_n = n;
    s_cache_mask = s_items;
    return n;
}

static bool layout_matches(void)
{
    if (s_need_rebuild || s_cache_mask != s_items) {
        return false;
    }
    int n = 0;
    for (int i = 0; i < device_count() && n < 8; i++) {
        const device_t *d = device_at(i);
        if (d == NULL || d->kind != DEV_OUT) {
            continue;
        }
        if (n >= s_cache_n ||
            strcmp(s_cache_id[n], d->id) != 0 ||
            strcmp(s_cache_name[n], d->name) != 0) {
            return false;
        }
        n++;
    }
    return n == s_cache_n;
}

/** 按当前页面/通知状态显示内容区或二维码页 */
static void ui_apply_visibility(void)
{
    bool show_qr = s_show_qr && (s_items & UI_SCREEN_QR) && !s_notify_active;
    if (s_content != NULL) {
        if (s_notify_active || show_qr) {
            lv_obj_add_flag(s_content, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(s_content, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_qr_content != NULL) {
        if (show_qr) {
            lv_obj_remove_flag(s_qr_content, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_qr_content, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (s_notify_panel != NULL) {
        if (s_notify_active) {
            lv_obj_remove_flag(s_notify_panel, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_add_flag(s_notify_panel, LV_OBJ_FLAG_HIDDEN);
        }
    }
}

/** 更新数值/状态（不重建控件） */
static void ui_update_values(const net_manager_status_t *st)
{
    char buf[48];

    if (s_lbl_clock != NULL) {
        if (s_items & UI_SCREEN_DATETIME) {
            time_t now = time(NULL);
            struct tm tm;
            if (now > 1700000000 && localtime_r(&now, &tm) != NULL) {
                snprintf(buf, sizeof(buf), "%02d:%02d", tm.tm_hour, tm.tm_min);
            } else {
                snprintf(buf, sizeof(buf), "--:--");
            }
        } else {
            snprintf(buf, sizeof(buf), "%s", net_manager_backend_name(st->active));
        }
        lv_label_set_text(s_lbl_clock, buf);
    }

    if (s_lbl_t_val != NULL) {
        float t, h;
        if (latest_temp_hum(&t, &h)) {
            snprintf(buf, sizeof(buf), "%.1f", t);
            lv_label_set_text(s_lbl_t_val, buf);
            snprintf(buf, sizeof(buf), "%.1f", h);
            lv_label_set_text(s_lbl_h_val, buf);
            lv_obj_set_style_text_color(s_lbl_t_val, lv_color_hex(0x69F0AE), LV_PART_MAIN);
            lv_obj_set_style_text_color(s_lbl_h_val, lv_color_hex(0x4FC3F7), LV_PART_MAIN);
        } else {
            lv_label_set_text(s_lbl_t_val, "--");
            lv_label_set_text(s_lbl_h_val, "--");
            lv_obj_set_style_text_color(s_lbl_t_val, lv_color_hex(0x607D8B), LV_PART_MAIN);
            lv_obj_set_style_text_color(s_lbl_h_val, lv_color_hex(0x607D8B), LV_PART_MAIN);
        }
    }

    for (int i = 0; i < s_sw_shown; i++) {
        const device_t *d = device_find(s_sw_id[i]);
        if (d == NULL) {
            lv_label_set_text(s_sw_state[i], "--");
            continue;
        }
        lv_label_set_text(s_sw_state[i], d->on ? "开" : "关");
        lv_obj_set_style_text_color(s_sw_state[i],
            lv_color_hex(d->on ? 0x69F0AE : 0xEF5350), LV_PART_MAIN);
        lv_obj_set_style_bg_color(s_sw_led[i],
            lv_color_hex(d->on ? 0x00E676 : 0xE53935), LV_PART_MAIN);
    }

    for (int i = 0; i < s_info_n; i++) {
        switch (s_info_kind[i]) {
        case 0:
            snprintf(buf, sizeof(buf), "IP %s", st->active_ip[0] ? st->active_ip : "-");
            break;
        case 1:
            snprintf(buf, sizeof(buf), "上行 %s", net_manager_backend_name(st->active));
            break;
        case 2:
            snprintf(buf, sizeof(buf), "ETH %s  WiFi %s",
                     st->eth_ready ? "OK" : "--", st->wifi_ready ? "OK" : "--");
            break;
        case 3: {
            uint64_t sec = (esp_timer_get_time() - s_boot_us) / 1000000ULL;
            snprintf(buf, sizeof(buf), "运行 %02llu:%02llu:%02llu",
                     sec / 3600, (sec % 3600) / 60, sec % 60);
            break;
        }
        default: {
            time_t now = time(NULL);
            struct tm tm;
            if (now > 1700000000 && localtime_r(&now, &tm) != NULL) {
                snprintf(buf, sizeof(buf), "%04d-%02d-%02d",
                         tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
            } else {
                snprintf(buf, sizeof(buf), "---- -- --");
            }
            break;
        }
        }
        lv_label_set_text(s_info_lbl[i], buf);
    }

#if defined(CONFIG_LV_USE_QRCODE)
    if (s_qr_obj != NULL) {
        char url[64];
        if (st->active_ip[0] != '\0') {
            snprintf(url, sizeof(url), "http://%s/", st->active_ip);
        } else {
            snprintf(url, sizeof(url), "http://esp32gw.local/");
        }
        if (strcmp(url, s_qr_url) != 0) {
            snprintf(s_qr_url, sizeof(s_qr_url), "%s", url);
            lv_qrcode_set_data(s_qr_obj, s_qr_url);
            if (s_qr_label != NULL) {
                lv_label_set_text(s_qr_label, s_qr_url);
            }
        }
    }
#endif
}

/** 重建整个看板（配置/设备列表变化时） */
static void ui_rebuild(const net_manager_status_t *st)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_clean(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x060B14), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    memset(s_info_lbl, 0, sizeof(s_info_lbl));
    memset(s_sw_row, 0, sizeof(s_sw_row));
    memset(s_sw_led, 0, sizeof(s_sw_led));
    memset(s_sw_name, 0, sizeof(s_sw_name));
    memset(s_sw_state, 0, sizeof(s_sw_state));
    s_info_n = 0;
    s_sw_shown = 0;
    s_lbl_clock = s_lbl_t_val = s_lbl_h_val = NULL;
    s_qr_content = NULL;
    s_qr_obj = NULL;
    s_qr_label = NULL;

    s_content = lv_obj_create(scr);
    lv_obj_set_size(s_content, UI_W, UI_H);
    lv_obj_set_pos(s_content, 0, 0);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_content, 0, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_content, 0, LV_PART_MAIN);
    lv_obj_set_scrollbar_mode(s_content, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);

    /* 顶栏 */
    lv_obj_t *hdr = ui_panel(s_content, 0, 0, UI_W, 22, 0x0E1A2F, 0);
    lv_obj_set_style_bg_grad_color(hdr, lv_color_hex(0x13233D), LV_PART_MAIN);
    lv_obj_t *title = ui_label(s_content, 7, 3, &lv_font_cjk_14, 0x00E5FF, "双网网关");
    (void)title;
    s_lbl_clock = ui_label(s_content, 0, 4, &lv_font_montserrat_12, 0x9FB3C8, "--:--");
    lv_obj_align(s_lbl_clock, LV_ALIGN_TOP_RIGHT, -7, 4);

    int y = 26;

    /* 温湿度卡片 */
    if (s_items & UI_SCREEN_TEMP_HUM) {
        lv_obj_t *ct = ui_panel(s_content, 4, y, 58, 48, 0x0E2422, 4);
        lv_obj_t *ch = ui_panel(s_content, 66, y, 58, 48, 0x0D1E30, 4);
        lv_obj_set_style_border_side(ct, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
        lv_obj_set_style_border_width(ct, 3, LV_PART_MAIN);
        lv_obj_set_style_border_color(ct, lv_color_hex(0x00E676), LV_PART_MAIN);
        lv_obj_set_style_border_side(ch, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
        lv_obj_set_style_border_width(ch, 3, LV_PART_MAIN);
        lv_obj_set_style_border_color(ch, lv_color_hex(0x29B6F6), LV_PART_MAIN);
        ui_label(ct, 8, 2, &lv_font_cjk_14, 0x78909C, "温度");
        ui_label(ch, 8, 2, &lv_font_cjk_14, 0x78909C, "湿度");
        s_lbl_t_val = ui_label(ct, 8, 19, &lv_font_montserrat_16, 0x69F0AE, "--");
        s_lbl_h_val = ui_label(ch, 8, 19, &lv_font_montserrat_16, 0x4FC3F7, "--");
        ui_label(ct, 44, 24, &lv_font_cjk_14, 0x78909C, "℃");
        ui_label(ch, 44, 24, &lv_font_cjk_14, 0x78909C, "%");
        y += 48 + 4;
    }

    /* 开关列表：优先占用可用空间 */
    int total_sw = 0;
    if (s_items & UI_SCREEN_SWITCHES) {
        for (int i = 0; i < device_count(); i++) {
            const device_t *d = device_at(i);
            if (d != NULL && d->kind == DEV_OUT) {
                total_sw++;
            }
        }
    }
    int slots = total_sw > 3 ? 3 : total_sw;
    int avail = 158 - y;
    int info_room = avail - (slots > 0 ? 14 : 0) - slots * 17;
    while (slots > 0 && info_room < 0) {
        slots--;
        info_room = avail - (slots > 0 ? 14 : 0) - slots * 17;
    }
    if (info_room < 0) {
        info_room = 0;
    }
    int info_max = info_room / 16;
    if (info_max > 5) {
        info_max = 5;
    }

    if (slots > 0) {
        char head[24];
        if (total_sw > slots) {
            snprintf(head, sizeof(head), "开关 +%d", total_sw - slots);
        } else {
            snprintf(head, sizeof(head), "开关");
        }
        ui_label(s_content, 7, y, &lv_font_cjk_14, 0x78909C, head);
        y += 14;
        int n = 0;
        for (int i = 0; i < device_count() && n < slots; i++) {
            const device_t *d = device_at(i);
            if (d == NULL || d->kind != DEV_OUT) {
                continue;
            }
            lv_obj_t *row = ui_panel(s_content, 6, y + n * 17, 116, 16, 0x121B2B, 3);
            s_sw_row[n] = row;
            s_sw_led[n] = lv_obj_create(row);
            lv_obj_set_size(s_sw_led[n], 6, 6);
            lv_obj_set_pos(s_sw_led[n], 5, 5);
            lv_obj_set_style_radius(s_sw_led[n], 3, LV_PART_MAIN);
            lv_obj_set_style_border_width(s_sw_led[n], 0, LV_PART_MAIN);
            lv_obj_set_style_bg_color(s_sw_led[n], lv_color_hex(0x546E7A), LV_PART_MAIN);
            lv_obj_remove_flag(s_sw_led[n], LV_OBJ_FLAG_SCROLLABLE);

            char name[20];
            utf8_copy(name, sizeof(name), d->name, 18);
            s_sw_name[n] = ui_label(row, 14, 0, &lv_font_cjk_14, 0xE0E0E0, name);
            lv_obj_set_width(s_sw_name[n], 74);
            s_sw_state[n] = ui_label(row, 0, 0, &lv_font_cjk_14, 0xEF5350, "关");
            lv_obj_align(s_sw_state[n], LV_ALIGN_RIGHT_MID, -5, 0);
            snprintf(s_sw_id[n], sizeof(s_sw_id[n]), "%s", d->id);
            n++;
        }
        s_sw_shown = n;
        y += slots * 17;
    }

    /* 信息行：IP / 上行 / 链路 / 运行 / 日期 */
    int kinds[5];
    int kn = 0;
    if (s_items & UI_SCREEN_IP)       kinds[kn++] = 0;
    if (s_items & UI_SCREEN_DATETIME) kinds[kn++] = 4;
    if (s_items & UI_SCREEN_UPTIME)   kinds[kn++] = 3;
    if (s_items & UI_SCREEN_UPLINK)   kinds[kn++] = 1;
    if (s_items & UI_SCREEN_LINK)     kinds[kn++] = 2;

    int info_n = kn < info_max ? kn : info_max;
    for (int i = 0; i < info_n; i++) {
        s_info_kind[i] = kinds[i];
        s_info_lbl[i] = ui_label(s_content, 7, y + i * 16, &lv_font_cjk_14, 0xB0BEC5, "");
        lv_obj_set_width(s_info_lbl[i], 116);
    }
    s_info_n = info_n;

    /* 二维码页（与信息页轮播；由 ui_apply_visibility 控制显隐） */
#if defined(CONFIG_LV_USE_QRCODE)
    if (s_items & UI_SCREEN_QR) {
        s_qr_content = lv_obj_create(scr);
        lv_obj_set_size(s_qr_content, UI_W, UI_H);
        lv_obj_set_pos(s_qr_content, 0, 0);
        lv_obj_set_style_bg_color(s_qr_content, lv_color_hex(0x060B14), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(s_qr_content, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_width(s_qr_content, 0, LV_PART_MAIN);
        lv_obj_set_style_pad_all(s_qr_content, 0, LV_PART_MAIN);
        lv_obj_set_scrollbar_mode(s_qr_content, LV_SCROLLBAR_MODE_OFF);
        lv_obj_remove_flag(s_qr_content, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *qhdr = ui_panel(s_qr_content, 0, 0, UI_W, 22, 0x0E1A2F, 0);
        (void)qhdr;
        ui_label(s_qr_content, 7, 3, &lv_font_cjk_14, 0x00E5FF, "扫码控制");

        s_qr_obj = lv_qrcode_create(s_qr_content);
        lv_qrcode_set_size(s_qr_obj, 104);
        lv_qrcode_set_dark_color(s_qr_obj, lv_color_black());
        lv_qrcode_set_light_color(s_qr_obj, lv_color_white());
        lv_qrcode_set_quiet_zone(s_qr_obj, true);
        lv_obj_set_pos(s_qr_obj, 12, 24);

        s_qr_label = ui_label(s_qr_content, 0, 132, &lv_font_montserrat_12, 0xE0E0E0, s_qr_url);
        lv_obj_set_width(s_qr_label, UI_W);
        lv_obj_set_style_text_align(s_qr_label, LV_TEXT_ALIGN_CENTER, LV_PART_MAIN);

        s_qr_url[0] = '\0';
        lv_obj_add_flag(s_qr_content, LV_OBJ_FLAG_HIDDEN);
    }
#endif

    /* HA 通知 banner（最后创建，位于顶层） */
    s_notify_panel = lv_obj_create(scr);
    lv_obj_set_size(s_notify_panel, 116, 110);
    lv_obj_set_pos(s_notify_panel, 6, 24);
    lv_obj_set_style_bg_color(s_notify_panel, lv_color_hex(0x1A0F12), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_notify_panel, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_border_side(s_notify_panel, LV_BORDER_SIDE_LEFT, LV_PART_MAIN);
    lv_obj_set_style_border_width(s_notify_panel, 4, LV_PART_MAIN);
    lv_obj_set_style_border_color(s_notify_panel, lv_color_hex(0x4FC3F7), LV_PART_MAIN);
    lv_obj_set_style_radius(s_notify_panel, 4, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_notify_panel, 6, LV_PART_MAIN);
    lv_obj_add_flag(s_notify_panel, LV_OBJ_FLAG_HIDDEN);

    s_notify_title = ui_label(s_notify_panel, 8, 4, &lv_font_cjk_14, 0x4FC3F7, "HA NOTIFY");
    s_notify_msg = ui_label(s_notify_panel, 8, 30, &lv_font_cjk_14, 0xFFFFFF, "-");
    lv_obj_set_width(s_notify_msg, 92);
    lv_label_set_long_mode(s_notify_msg, LV_LABEL_LONG_WRAP);

    collect_out_cache();
    s_need_rebuild = false;

    if (s_notify_active) {
        lv_label_set_text(s_notify_title, s_last_notify.title[0] ? s_last_notify.title : "HA NOTIFY");
        lv_label_set_text(s_notify_msg, s_last_notify.message[0] ? s_last_notify.message : "-");
    }
    ui_apply_visibility();

    ui_update_values(st);
}

/** 刷新看板内容（必须在持 LVGL 锁的情况下调用） */
static void ui_refresh(const net_manager_status_t *st, bool uptime_only)
{
    (void)uptime_only;
    if (!layout_matches()) {
        ui_rebuild(st);
    } else {
        ui_update_values(st);
    }
}

/* ---------------------------------------------------------------------------
 * HA 通知：显示 / 恢复（必须在持 lvgl_port_lock 或由本文件内部保证的情况下调用）
 * ------------------------------------------------------------------------- */

/** 显示 HA 通知 banner（覆盖 KPI 区，标题与左边框按 level 着色） */
static void ui_show_notify(const ha_notify_t *n)
{
    if (!lvgl_port_lock(UI_LVGL_LOCK_TIMEOUT_MS)) {
        return;
    }
    /* level 主题色：info 浅蓝 / warn 橙 / error 红 */
    uint32_t c = 0x4FC3F7;
    if (n->level == HA_NOTIFY_WARN) {
        c = 0xFF9800;
    } else if (n->level == HA_NOTIFY_ERROR) {
        c = 0xF44336;
    }
    lv_obj_set_style_border_color(s_notify_panel, lv_color_hex(c), LV_PART_MAIN);
    lv_obj_set_style_text_color(s_notify_title, lv_color_hex(c), LV_PART_MAIN);
    lv_label_set_text(s_notify_title, n->title[0] ? n->title : "HA NOTIFY");
    lv_label_set_text(s_notify_msg, n->message[0] ? n->message : "-");
    s_last_notify = *n;

    s_notify_active   = true;
    ui_apply_visibility();
    s_notify_until_us = esp_timer_get_time() +
                        (uint64_t)CONFIG_HA_MQTT_NOTIFY_SHOW_MS * 1000ULL;
    lvgl_port_unlock();
}

/** 恢复常规状态显示（由周期刷新任务检测超时后调用） */
static void ui_hide_notify(void)
{
    s_notify_active = false;
    ui_apply_visibility();
}

void ui_dashboard_show_home(void)
{
    if (!lvgl_port_lock(UI_LVGL_LOCK_TIMEOUT_MS)) {
        return;
    }
    s_show_qr = false;
    s_notify_active = false;
    s_page_switch_us = esp_timer_get_time();
    s_qr_paused_until_us = s_page_switch_us + 10LL * 60 * 1000 * 1000;
    ui_apply_visibility();
    lvgl_port_unlock();
    ESP_LOGI(TAG, "返回信息桌面（二维码轮播暂停 10 分钟）");
}

/** BOOT 长按检测（GPIO0 低电平有效） */
static void boot_button_task(void *arg)
{
    (void)arg;
    bool pressed = false;
    bool fired = false;
    int64_t press_us = 0;

    for (;;) {
        int level = gpio_get_level(BOOT_GPIO_NUM);
        int64_t now = esp_timer_get_time();
        if (level == 0) {
            if (!pressed) {
                pressed = true;
                fired = false;
                press_us = now;
            } else if (!fired && now - press_us >= (int64_t)BOOT_LONG_PRESS_MS * 1000) {
                fired = true;
                ui_dashboard_show_home();
            }
        } else {
            pressed = false;
        }
        vTaskDelay(pdMS_TO_TICKS(BOOT_POLL_MS));
    }
}

/* ---------------------------------------------------------------------------
 * 数据源 3：MQTT 传感器温湿度（作为自定义传感器缺失时的兜底）
 * ------------------------------------------------------------------------- */
static void ui_sensor_handler(const ha_sensor_t *sn, void *user_ctx)
{
    (void)user_ctx;
    s_sensor = *sn;
    s_sensor_has = true;
}

/* 板内告警：直接弹通知横幅（内部自取 LVGL 锁，可在任意任务调用） */
void ui_dashboard_show_notify(const char *title, const char *message, int level)
{
    ha_notify_t n;
    memset(&n, 0, sizeof(n));
    snprintf(n.title, sizeof(n.title), "%s", title ? title : "");
    snprintf(n.message, sizeof(n.message), "%s", message ? message : "");
    n.level = (level >= 2) ? HA_NOTIFY_ERROR
            : (level == 1) ? HA_NOTIFY_WARN : HA_NOTIFY_INFO;
    ui_show_notify(&n);
}

/* ---------------------------------------------------------------------------
 * 数据源 1：net_manager 链路切换监听器（即时刷新）
 * 注意：该回调在 net_manager 任务内执行（其互斥锁被持有），此处只做快速更新。
 * ------------------------------------------------------------------------- */
static void ui_net_listener(net_manager_status_t *st, void *user_ctx)
{
    (void)user_ctx;
    if (!lvgl_port_lock(UI_LVGL_LOCK_TIMEOUT_MS)) {
        return;    /* 锁获取失败：跳过本次刷新（下一轮任务兜底） */
    }
    ui_refresh(st, false);
    lvgl_port_unlock();
}

/* ---------------------------------------------------------------------------
 * 数据源 2：周期刷新任务（运行时长每秒刷新；网络状态兜底刷新）
 * ------------------------------------------------------------------------- */
static void ui_refresh_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(UI_REFRESH_MS));

        /* 先取网络快照，再加锁刷 UI：避免持 LVGL 锁去取网络状态造成锁序死锁 */
        net_manager_status_t st = net_manager_get_status();
        if (!lvgl_port_lock(UI_LVGL_LOCK_TIMEOUT_MS)) {
            continue;
        }
        ui_refresh(&st, false);

        /* 二维码页与信息页每 10s 轮播 */
        if ((s_items & UI_SCREEN_QR) && !s_notify_active &&
            esp_timer_get_time() >= s_qr_paused_until_us) {
            int64_t now = esp_timer_get_time();
            if (s_page_switch_us == 0) {
                s_page_switch_us = now;
            } else if (now - s_page_switch_us >= 10 * 1000 * 1000LL) {
                s_show_qr = !s_show_qr;
                s_page_switch_us = now;
                ui_apply_visibility();
            }
        }

        /* 通知显示窗口到期 → 恢复常规内容 */
        if (s_notify_active && esp_timer_get_time() >= s_notify_until_us) {
            ui_hide_notify();
            ESP_LOGI(TAG, "通知显示到期，恢复常规状态");
        }
        lvgl_port_unlock();
    }
}

/* ---------------------------------------------------------------------------
 * HA 通知入口：由 ha_mqtt 投递任务调用（非 MQTT 回调），内部加 LVGL 锁
 * ------------------------------------------------------------------------- */
static void ui_ha_notify_handler(const ha_notify_t *notify, void *user_ctx)
{
    (void)user_ctx;
    ui_show_notify(notify);
}

/* ---------------------------------------------------------------------------
 * 对外接口：看板初始化
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * LCD 画面快照（Web 控制台 /api/screen.bmp 用）
 *   持 LVGL 锁拷贝当前帧缓冲，避免与刷新竞争；内存取自 PSRAM。
 * ------------------------------------------------------------------------- */
uint8_t *ui_dashboard_copy_framebuffer(int *w, int *h, int *stride)
{
    if (s_disp == NULL || !lvgl_port_lock(UI_LVGL_LOCK_TIMEOUT_MS)) {
        return NULL;
    }
    uint8_t *out = NULL;
    lv_draw_buf_t *db = lv_display_get_buf_active(s_disp);
    if (db != NULL && db->data != NULL) {
        int W = db->header.w;
        int H = db->header.h;
        int S = db->header.stride;
        size_t sz = (size_t)S * (size_t)H;
        out = heap_caps_malloc(sz, MALLOC_CAP_SPIRAM);
        if (out != NULL) {
            memcpy(out, db->data, sz);
            if (w)      *w = W;
            if (h)      *h = H;
            if (stride) *stride = S;
        }
    }
    lvgl_port_unlock();
    return out;
}

uint32_t ui_dashboard_get_items(void)
{
    return s_items;
}

esp_err_t ui_dashboard_set_items(uint32_t mask)
{
    s_items = mask & UI_SCREEN_ALL;
    if (!(s_items & UI_SCREEN_QR)) {
        s_show_qr = false;
    }
    s_need_rebuild = true;
    nvs_handle_t h;
    if (nvs_open(UI_ITEMS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u32(h, "mask", s_items);
        nvs_commit(h);
        nvs_close(h);
    }
    return ESP_OK;
}

esp_err_t ui_dashboard_get_text(char *buf, size_t size)
{
    if (buf == NULL || size == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    net_manager_status_t st = net_manager_get_status();
    compose_body(buf, size, &st);
    return ESP_OK;
}

static void ui_load_items(void)
{
    nvs_handle_t h;
    uint32_t mask = s_items;
    if (nvs_open(UI_ITEMS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u32(h, "mask", &mask);
        nvs_close(h);
    }
    s_items = mask & UI_SCREEN_ALL;
}

esp_err_t ui_dashboard_init(void)
{
    s_boot_us = esp_timer_get_time();
    ui_load_items();

    /* 1) ST7735S 面板：SPI3 独立总线；RST 恒 -1（与 W5500 共用，不重复复位）。
          必须在 net_eth_init() 之后调用 —— app_main 顺序已保证） */
    lcd_st7735_config_t lcd_cfg = lcd_st7735_default_config();
    esp_lcd_panel_io_handle_t io = NULL;
    esp_lcd_panel_handle_t panel = NULL;
    ESP_RETURN_ON_ERROR(lcd_st7735_init(&lcd_cfg, &io, &panel),
                        TAG, "ST7735S 面板创建失败");

    /* 软件复位（SWRESET 命令，不触碰 GPIO15）+ 写入初始化命令序列 */
    ESP_RETURN_ON_ERROR(esp_lcd_panel_reset(panel), TAG, "面板复位失败");
    ESP_RETURN_ON_ERROR(esp_lcd_panel_init(panel), TAG, "面板初始化失败");

#if defined(CONFIG_LCD_ST7735_TEST_PATTERN)
    /* == 诊断模式：绕过 LVGL，只绘制测试图（供校准 ROTATION / OFFSET / 裁切 / 斜切）== */
    ESP_RETURN_ON_ERROR(lcd_st7735_draw_test_pattern(panel), TAG, "测试图绘制失败");
    ESP_LOGI(TAG, "测试图诊断模式运行中（未启动 LVGL 看板，调试完成后关闭 TEST_PATTERN）");
    return ESP_OK;
#endif

    /* 2) 旋转后的分辨率（决定 LVGL 显示尺寸与缓冲大小）
          （注意：控件布局按 128x160 竖屏优化；旋转横屏下仅显示缓冲自适应） */
    ESP_RETURN_ON_ERROR(lcd_st7735_get_resolution(panel, &s_ui_w, &s_ui_h),
                        TAG, "获取面板分辨率失败");

    /* 3) LVGL 端口初始化（内部 lv_init + 创建 LVGL 任务） */
    ESP_RETURN_ON_ERROR(lvgl_port_init(&(lvgl_port_cfg_t)ESP_LVGL_PORT_INIT_CONFIG()),
                        TAG, "LVGL 端口初始化失败");

    /* 4) 挂接 ST7735S 为 LVGL 显示设备：
          - 单缓冲整帧（w*h*2 = 40KB，DMA 内存）
          - swap_bytes：ST7735S 16bpp 像素为大端字节序，发送前端口负责交换 */
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = io,               /* add_disp 内部会注册传输完成回调，必须提供 */
        .panel_handle  = panel,
        .buffer_size   = s_ui_w * s_ui_h,
        .double_buffer = false,
        .hres          = s_ui_w,
        .vres          = s_ui_h,
        /* 【关键】旋转必须与 lcd_st7735 的 Kconfig 旋转保持一致！
           esp_lvgl_port 在 add_disp() 时会调用 esp_lcd_panel_mirror()/swap_xy()，
           而本驱动的 mirror/swap_xy 会【重写】MADCTL —— 若这里填 false/false，
           会把 lcd_st7735 初始化时设好的 ROTATION_180(MX|MY) 覆盖掉，
           导致看板上下颠倒（而直接绘制的测试图仍正常，症状极具迷惑性）。 */
        .rotation      = {
#if CONFIG_LCD_ST7735_ROTATION_90
            .swap_xy = true,  .mirror_x = false, .mirror_y = false,
#elif CONFIG_LCD_ST7735_ROTATION_180
            .swap_xy = false, .mirror_x = true,  .mirror_y = true,
#elif CONFIG_LCD_ST7735_ROTATION_270
            .swap_xy = true,  .mirror_x = true,  .mirror_y = true,
#else /* ROTATION_0 */
            .swap_xy = false, .mirror_x = false, .mirror_y = false,
#endif
        },
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .flags = {
            .buff_dma     = true,
            .buff_spiram  = true,   /* 缓冲放 PSRAM：WiFi 连接后会占满内部 DMA RAM，
                                       40KB 内部分配会失败（实测踩过：导致重启循环）*/
            .swap_bytes   = true,
            .sw_rotate    = false,
            .full_refresh = true,
        },
    };
    lv_display_t *disp = lvgl_port_add_disp(&disp_cfg);
    if (disp == NULL) {
        ESP_LOGE(TAG, "LVGL 显示设备挂接失败");
        return ESP_FAIL;
    }
    s_disp = disp;
    ESP_LOGI(TAG, "LVGL 显示设备挂接成功（%lux%lu RGB565，%luKB 缓冲）",
             (unsigned long)s_ui_w, (unsigned long)s_ui_h,
             (unsigned long)(s_ui_w * s_ui_h * 2 / 1024));

    /* 5) 创建控件并做首次刷新（快照在锁外获取，避免锁序问题） */
    net_manager_status_t st0 = net_manager_get_status();
    if (!lvgl_port_lock(UI_LVGL_LOCK_TIMEOUT_MS)) {
        ESP_LOGE(TAG, "LVGL 锁获取失败");
        return ESP_FAIL;
    }
    ui_rebuild(&st0);
    lvgl_port_unlock();

    /* 6) 注册链路切换监听器（上行变化即时刷新）+ 周期刷新任务 */
    net_manager_register_listener(ui_net_listener, NULL);
    xTaskCreate(ui_refresh_task, "ui_refresh", UI_TASK_STACK, NULL, UI_TASK_PRIO, &s_task);

    /* 6.1) BOOT 长按：返回信息桌面 */
    gpio_config_t boot_cfg = {
        .pin_bit_mask = 1ULL << BOOT_GPIO_NUM,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&boot_cfg);
    xTaskCreate(boot_button_task, "boot_btn", BOOT_TASK_STACK, NULL,
                BOOT_TASK_PRIO, NULL);

    /* 7) 注册 HA 通知处理器（ha_mqtt 投递任务上下文调用，内部加 LVGL 锁） */
    ha_mqtt_register_notify_handler(ui_ha_notify_handler, NULL);
    ha_mqtt_register_sensor_handler(ui_sensor_handler, NULL);
    ESP_LOGI(TAG, "已订阅 HA 通知（显示时长 %d ms）", CONFIG_HA_MQTT_NOTIFY_SHOW_MS);

    ESP_LOGI(TAG, "本地看板启动完成（数据来自 net_manager）");
    return ESP_OK;
}