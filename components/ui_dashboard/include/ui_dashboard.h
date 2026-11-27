/**
 * @file ui_dashboard.h
 * @brief LVGL 本地看板（ST7735S 128x160：双网状态看板）对外接口
 */
#pragma once

#include <stdint.h>

#include "esp_err.h"

/* 屏幕显示项位掩码（Web 屏幕设置页使用） */
#define UI_SCREEN_TEMP_HUM  (1u << 0)   /* 温度/湿度 */
#define UI_SCREEN_SWITCHES  (1u << 1)   /* 已定义开关状态 */
#define UI_SCREEN_IP        (1u << 2)   /* 当前 IP */
#define UI_SCREEN_DATETIME  (1u << 3)   /* 日期时间 */
#define UI_SCREEN_UPLINK    (1u << 4)   /* 上行链路 */
#define UI_SCREEN_UPTIME    (1u << 5)   /* 运行时长 */
#define UI_SCREEN_LINK      (1u << 6)   /* ETH/WiFi 可用性 */
#define UI_SCREEN_QR        (1u << 7)   /* 二维码页（与信息页轮播） */
#define UI_SCREEN_ALL       (0xFFu)

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 启动本地看板（初始化 ST7735S 面板 + LVGL + 看板 UI + 状态刷新）
 *
 * @note 必须在 net_eth_init() 之后调用（LCD RST 与 W5500 共用 GPIO15，
 *       上电复位由 net_eth 完成，本组件不驱动该引脚）。
 */
esp_err_t ui_dashboard_init(void);

/**
 * @brief 取当前 LCD 画面快照（RGB565，调用者负责 free）
 *
 * 内部持 LVGL 锁拷贝，避免与刷新竞争；失败返回 NULL。
 * @param[out] w 宽
 * @param[out] h 高
 * @param[out] stride 行字节数（= w*2）
 */
uint8_t *ui_dashboard_copy_framebuffer(int *w, int *h, int *stride);

/**
 * @brief 在看板上显示一条本地通知（板内告警用，如传感器掉线）
 * @param level 0=info 1=warn 2=error
 */
void ui_dashboard_show_notify(const char *title, const char *message, int level);

/** @brief 读取/设置屏幕显示项（保存到 NVS，下一轮刷新生效） */
uint32_t ui_dashboard_get_items(void);
esp_err_t ui_dashboard_set_items(uint32_t mask);

/** @brief 取当前屏幕文本内容（诊断/预览用，非 LVGL 操作） */
esp_err_t ui_dashboard_get_text(char *buf, size_t size);

#ifdef __cplusplus
}
#endif