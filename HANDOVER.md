# ESP32-S3 网关交接文档

> 交接日期：2026-11-25
> 固件提交：`5c2246c`（`main` 与 `suhongbo2017/omp-continue` 均已推送）
> 设备：ESP32-S3 + W5500 + ST7735S 128×160 + TF 卡
> 现场访问：`http://192.168.99.13/`（mDNS `esp32gw.local`），串口 **COM7**，MQTT broker `192.168.99.253:1883`

---

## 1. 当前状态摘要

| 项目 | 状态 |
|---|---|
| 构建 | ESP-IDF v5.5.5，`idf.py build` 通过；镜像 2,338,416 字节（3MB app 分区约余 25%） |
| 网络 | 当前 WiFi 上行 `192.168.99.13`；ETH 驱动正常但现场未插网线 |
| Web | 三页：信息 / 设置与升级 / GPIO 定义；无 Token 鉴权 |
| 本地屏 | 卡片式 UI：顶栏时钟、温湿度大卡、开关列表、信息行；可轮播控制页二维码 |
| 设备定义 | 运行时定义并持久化：开关、DHT22、SHT31/SHT4x、BH1750、ADC 声音 |
| HA | MQTT Discovery 自动生成 switch / sensor 实体；状态双向同步 |
| 长稳 | 最近一次启动后运行正常；历史上有 17h 卡死问题，已修复 LVGL 锁无限等待 |

当前屏幕配置掩码 **255**（全部项，含二维码）；信息页与二维码页每 10s 轮播。屏幕文本 `GET /api/screen/text` 预览，画面 `GET /api/screen.bmp` 下载。

---

## 2. 关键代码位置

```
components/
├── device_registry/   # 设备定义 + NVS + I2C/ADC/DHT 驱动 + 1s 轮询任务
├── web_console/       # HTTP API + 内嵌三页网页（www_index.h）
├── ha_mqtt/           # MQTT 连接、Discovery、命令、状态、事件队列
├── ui_dashboard/      # LVGL 看板 + 可配置内容 + 通知横幅
├── net_eth/net_wifi/net_manager/  # 双网
├── tf_storage/ sensor_history/   # TF 日志与温湿度曲线（仍为旧 MQTT 节点数据）
└── lcd_st7735/ board/ app_cfg/ market_data/
main/app_main.c        # 启动顺序：NVS→cfg→设备注册表→双网→TF→MQTT→LCD→Web
```

### 线程模型（重要）

- `device_registry` 轮询任务固定在 **core 1**，周期 1s；DHT22 实际间隔 2.5s。
- DHT22 读取在临界区内约 4ms，避免中断错过时序；不要把它改到 core 0。
- Web / MQTT 线程**不直接**发布设备状态：通过 `ha_dev` 队列 + 独立任务发布，避免 HTTP 任务卡在 MQTT。
- LVGL 操作必须持 `lvgl_port_lock(20ms)`；禁止持 LVGL 锁调用 `net_manager_get_status()`（ABBA 死锁）。

---

## 3. 设备与接线

### 3.1 当前现场定义（NVS `devdefs`）

| id | 名称 | 类型 | 引脚/地址 | 说明 |
|---|---|---|---|---|
| `gpio41` | 继电器1 | 开关输出 | GPIO41 | 电灯开关 |
| `gpio4` | 箱内灯 | 开关输出 | GPIO4 | 继电器 IN，低电平关 |
| `dht42` | 箱内温湿度 | DHT22 | GPIO42 | 当前间歇无应答，待加 4.7k–10k 上拉 |

### 3.2 引脚规则

- 数字输出/输入：GPIO1–9、39–42、47–48（白名单，见 `device_registry.c`）。
- **ADC 只能用 GPIO1–9（ADC1）**；ADC2（GPIO11–20）在 WiFi 下不可用。
- I2C 默认 **SDA=GPIO47 / SCL=GPIO48**（可在 Web 修改并保存到 NVS `devbus`）。
- 板载占用（不可定义）：0、10–21、26–38、43–46；Web 以说明窗展示。
- 当前占用：4、41（开关），42（DHT），47/48（I2C 总线）。

### 3.3 传感器接线

| 类型 | 接线 |
|---|---|
| DHT22 三针模块 | VCC→3.3V，GND→GND，OUT→数字 GPIO（**加 4.7k–10k 上拉到 3.3V**） |
| SHT31/SHT4x | VCC→3.3V，GND，SDA/SCL→总线；地址 0x44（可改 0x45） |
| BH1750 | 地址 0x23（可改 0x5C） |
| 声音模块 | 模拟包络输出（AO）→ ADC1 GPIO1–9，输出为峰峰值 mV；不要接数字 DO |

> 现场 I2C 扫描曾短暂出现 `0x36`，之后为空；未验证 SHT/BH 实物。接线时先确认 4.7k 上拉与共地。

---

## 4. HTTP API（无鉴权，仅限可信局域网）

| 方法 | 路径 | 说明 |
|---|---|---|
| GET | `/` | 内嵌三页控制台 |
| GET | `/api/status` | 网络/内存/TF/MQTT/传感器状态 |
| GET | `/api/gpio` | 已定义设备、可用引脚、I2C 总线、占用引脚 |
| POST | `/api/gpio/define` | 定义设备，JSON `kind/model/pin/addr/name/desc` |
| POST | `/api/gpio/remove` | 删除设备，JSON `{"id"}`；会清理 HA 实体 |
| POST | `/api/gpio/bus` | 设置 I2C 总线，JSON `{"sda","scl"}` |
| POST | `/api/gpio/scan` | 扫描 I2C 地址 |
| POST | `/api/gpio/dht_test` | DHT22 诊断，JSON `{"pin"}`，返回电平/应答/校验 |
| GET/POST | `/api/screen` | 读取/设置屏幕显示掩码 `{"mask":N}` |
| GET | `/api/screen/text` | 预览当前屏幕文本 |
| GET | `/api/screen.bmp` | 下载当前 LCD 画面（BMP，24-bit） |
| POST | `/api/screen/home` | 屏幕返回信息桌面（等效 BOOT 长按） |
| POST | `/api/screen/qr` | 立即显示二维码页（等效 BOOT 短按；手动显示期间不轮播） |
| POST | `/api/device?id=&value=0\|1` | 开关输出控制 |
| GET | `/api/logs` / `/api/logs/<file>` | TF 日志列表/下载 |
| GET/POST | `/api/config` | NVS 配置读取/保存 |
| GET | `/api/history` | 温湿度曲线数据（旧 MQTT 节点） |
| POST | `/api/reboot` | 重启 |
| POST | `/api/ota` | 上传原始 bin 升级 |

定义示例：

```bash
# 开关
curl -X POST -H "Content-Type: application/json" \
  -d '{"kind":"out","pin":6,"name":"风扇","desc":"继电器"}' \
  http://192.168.99.13/api/gpio/define

# DHT22
curl -X POST -H "Content-Type: application/json" \
  -d '{"kind":"dht","pin":42,"name":"箱内温湿度","desc":"DHT22"}' \
  http://192.168.99.13/api/gpio/define

# SHT31 / BH1750
curl -X POST -H "Content-Type: application/json" \
  -d '{"kind":"i2c","model":"sht31","name":"温湿度","desc":""}' \
  http://192.168.99.13/api/gpio/define
```

屏幕掩码位：

| bit | 值 | 内容 |
|---|---|---|
| 0 | 1 | 温湿度 |
| 1 | 2 | 开关状态 |
| 2 | 4 | IP |
| 3 | 8 | 日期时间 |
| 4 | 16 | 上行链路 |
| 5 | 32 | 运行时长 |
| 6 | 64 | ETH/WiFi 可用性 |
| 7 | 128 | 二维码页（`http://<当前IP>/`，与信息页每 10s 轮播） |

信息页最多 8 行；开关过多自动截断。二维码页固定 10s，扫码可直接打开控制台。

**BOOT 按键**：短按（50ms–2s）切换二维码页/桌面；长按（≥2s）立即回到信息桌面、关闭通知并暂停二维码轮播 10 分钟。
Web 屏幕设置页有「显示二维码」和「屏幕返回桌面」按钮，等效上述操作；`/api/screen/qr`、`/api/screen/home` 可供脚本调用。

---

## 5. MQTT / Home Assistant

| 主题 | 方向 | 内容 |
|---|---|---|
| `ha/esp32gw/state` | 发布，retain | `{online,uplink,ip,uptime,notify_title,notify_message,http}`，心跳 5s；`http` 为最后处理的 Web 请求 |
| `ha/esp32gw/dev/<id>/set` | 订阅 | 开关注释：`ON/OFF/1/0` |
| `ha/esp32gw/dev/<id>/state` | 发布 | 开关 `ON/OFF`；传感器 JSON（`temp/hum`、`lux`、`level`） |
| `ha/esp32gw/notify` | 订阅 | `{title,message,level}` 看板通知 |
| `ha/esp32gw/alert` | 发布 | 传感器离线/恢复告警 |
| `esp32-sensor/data` | 订阅 | 旧温湿度节点 JSON |
| `homeassistant/switch/esp32gw_dev_<id>/config` | 发布，retain | 开关 Discovery |
| `homeassistant/sensor/esp32gw_dev_<id>_<metric>/config` | 发布，retain | 传感器 Discovery（每个指标一条） |

HA 设备名：`Sensor-SU-1`。删除设备会清空对应 Discovery/state 并退订命令主题。

---

## 6. NVS 布局

| namespace | key | 说明 |
|---|---|---|
| `devdefs` | `n`、`d0..d15` | 设备数量与 JSON：`{id,name,desc,kind,pin,model,addr}` |
| `devbus` | `sda`,`scl` | I2C 总线引脚（默认 47/48） |
| `uiscreen` | `mask` | 屏幕显示位掩码 |
| `gwcfg` | 多项 | WiFi/MQTT/主题等运行配置（`app_cfg`） |
| `nvs` | — | WiFi 驱动等 |

旧版开关记录没有 `kind/model/addr`，加载时按 `kind=0`（开关）兼容。

---

## 7. 构建 / 烧录 / OTA

| 项 | 值 |
|---|---|
| 工程（主） | `H:\VSCODE\ESP32-S3-Gateway`（main） |
| 工程（Orca） | `C:\Users\Subuntu\orca\workspaces\ESP32-S3-Gateway\goldeye`（分支 `suhongbo2017/omp-continue`） |
| ESP-IDF | `H:\esp\esp-idf`（v5.5.5） |
| 工具链 | `IDF_TOOLS_PATH=H:\espressif` |
| 串口 | **COM7**（CH340K，115200） |

```bat
:: 构建
set IDF_TOOLS_PATH=H:\espressif
cd /d H:\esp\esp-idf && call export.bat
cd /d <工程目录> && idf.py build

:: 串口烧录
idf.py -p COM7 flash

:: OTA（设备可访问时）
curl --noproxy "*" -X POST "http://192.168.99.13/api/ota" ^
  --data-binary "@build\esp32s3_gateway.bin"
```

`sdkconfig.defaults` 已包含**实测必需项**，新建 sdkconfig 时不要删：

- `CONFIG_LV_USE_CLIB_MALLOC=y`（避免 LVGL 64KB 内部内存池耗尽）
- `CONFIG_LV_USE_FONT_COMPRESSED=y`（CJK 字体压缩格式必需）
- `CONFIG_LCD_ST7735_ROTATION_180=y`、RGB（非 BGR）
- `CONFIG_FATFS_LFN_HEAP=y`、`CONFIG_FATFS_MAX_LFN=255`
- `CONFIG_WEB_CONSOLE_MDNS_HOSTNAME="esp32gw"`
- `CONFIG_LWIP_MAX_SOCKETS=24`（默认 10 会导致并发请求后 `accept errno 23`）

---

## 8. 已验证内容（截至交接）

- 三页 Web、类型化设备定义、删除二次确认、HA switch/sensor 同步。
- 开关：定义/删除/重启恢复/HA 状态双向。
- 声音 ADC：定义、1s 采样、HA Discovery/JSON 状态。
- DHT22：一次完整读取成功（23.5℃ / 61.9%，CRC 通过），随后出现间歇无应答；诊断接口可复现。
- 屏幕：掩码配置、NVS 重启保持、文本预览、8 行截断。
- 并发：10 路并发 HTTP 全部 200；lwIP socket 修复后无 `accept (23)`。
- 稳定性：HTTP 任务不再直接调 MQTT；LVGL 锁 20ms 超时；DHT 轮询栈溢出已修复。

未验证：SHT31/SHT4x/BH1750 实物读数（现场无稳定 I2C 设备）；声音模块实物（仅验证悬空 ADC 链路）；长稳 >24h。

---

## 9. 已知问题与风险

1. **DHT22 曾间歇无应答**：当前读数稳定（约 24.5℃/71%）；仍建议确认 4.7k–10k 上拉，长线/干扰可能导致偶发 CRC 失败。
2. **I2C 传感器未实物验证**：SHT/BH 驱动按标准协议实现；先用 `POST /api/gpio/scan` 确认地址。
3. **历史曲线仍是旧 MQTT 节点数据**：新定义的 DHT/SHT 读数尚未进入 `sensor_history` 与 TF CSV。
4. **无鉴权**：所有 `/api/*` 明文可调用；只能部署在可信隔离局域网，禁止公网暴露。
5. **内存偏紧**：当前 `heap_free` 约 63KB、最低水位 22KB；新增功能注意 PSRAM/内部 RAM 使用。
6. **屏幕最多 8 行**：开关名称截断、只显示第一个有效温湿度传感器；暂无翻页/排序。
7. **NVS 记录丢失历史**：升级过程中出现过一次 `gpio4` 定义丢失（已恢复）；当前加载日志会打印每条记录，若再出现请保留串口日志。
8. **诊断接口有副作用**：`dht_test` 会切换 GPIO 方向，运行期间会短暂干扰该设备的轮询。

---

## 10. 排障手册

| 现象 | 检查 |
|---|---|
| 页面 `Failed to fetch` | 串口是否出现 `httpd_accept_conn: error in accept (23)` → 确认 `CONFIG_LWIP_MAX_SOCKETS=24`；看 `/api/status` 的 `http` 字段定位最后请求 |
| 上电反复重启 | 串口 backtrace；曾因 HTTP 路由 > `max_uri_handlers` 触发 abort（已改为 24） |
| 设备 ping 不通但 MQTT 有心跳 | 看 `ha/esp32gw/state` 的 `ip/http`；可能 WiFi 重连中 |
| DHT 无应答 | `POST /api/gpio/dht_test {"pin":42}` 看 `idle_level/drive_low_level/release_level/resp_low_us`；补上拉 |
| I2C 无设备 | `POST /api/gpio/scan`；检查 SDA/SCL、上拉、供电、地址 |
| 定义 I2C 失败 | 先扫描确认地址；定义时会实际探测，无应答直接拒绝 |
| 屏幕内容不对 | `GET /api/screen` 看掩码，`GET /api/screen/text` 看文本，`GET /api/screen.bmp` 看画面；设置页重新勾选保存 |
| TF 不写 | 检查 `tf_storage` 日志、卡格式、长文件名配置 |

串口抓取（Windows）：

```bat
python -m serial.tools.miniterm COM7 115200
```

或使用 `idf.py -p COM7 monitor`（退出 `Ctrl+]`）。

---

## 11. 建议的下一步（按优先级）

1. **DHT22 稳定性**：加上拉并连续观察 ≥24h；失败率仍高则换传感器或改用 SHT31。
2. **传感器进历史**：把 `device_registry` 的温湿度/光照接入 `sensor_history` 与 TF CSV，Web 曲线支持选择设备。
3. **阈值告警**：高温/高湿/传感器离线 → HA 告警 + 屏幕横幅。
4. **I2C 实物验证**：SHT31 + BH1750 接入后验证读数、地址覆盖、总线扫描。
5. **屏幕增强**：选择具体传感器/开关、调整顺序、超过 8 行分页。
6. **安全**：跨网段部署前恢复鉴权（Token 或 HA 认证代理）。

---

## 12. Git / 交接检查清单

- `main`：`5c2246c`（已推送）
- `suhongbo2017/omp-continue`：`5c2246c`（已推送）
- 主工作区 `H:\VSCODE\ESP32-S3-Gateway` 干净；合并前旧改动保存在 `stash@{0}`（含 COM7 日志），一般无需恢复。
- 设备当前运行 `5c2246c` 固件。

接手后 10 分钟检查：

1. `GET /api/status` 返回 `ok:true`，`mqtt:true`，IP 正确。
2. `GET /api/gpio` 能看到 `gpio41/gpio4/dht42` 和 I2C 总线状态。
3. `GET /api/screen/text` 输出与 LCD 实际显示一致。
4. 串口启动无 `abort/Guru/stack overflow`。
5. 在 HA 里能看到 `Sensor-SU-1` 的开关/传感器实体并可控制。
