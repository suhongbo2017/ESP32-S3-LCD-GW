# ESP32-S3 双网冗余 IoT 网关 + 本地看板（方案 A）

> 板型：ESP32_S3_LCD 带屏幕（W5500 网口 + 1.77″ ST7735S TFT + TF 卡 + CH340K）
> 选型依据见同目录 [`SOLUTIONS.md`](./SOLUTIONS.md)，引脚依据见 [`PINMAP.md`](./PINMAP.md)

## 📚 项目文档索引

| 文档 | 内容 |
|------|------|
| [`README.md`](./README.md)（本文） | 架构、编译/烧录、里程碑、**§11 已确认参数（用户实测 OK）**、踩坑记录 |
| [`PINMAP.md`](./PINMAP.md) | 原厂电路板引脚映射（W5500/LCD/TF/串口）+ 实测方向与颜色配置 |
| [`SOLUTIONS.md`](./SOLUTIONS.md) | 方案选型（为何选方案 A） |
| [`DEVICES.md`](./DEVICES.md) | **模块化设备设计**—开关、ADC、I2C 传感器、DHT22 已实现；PWM/输入类按此扩展 |
| [`BUILD_FLOW.md`](./BUILD_FLOW.md) | **项目构建流水**：环境、配置、编译、烧录、OTA、验证、故障排查和大修改文档同步规则 |
| [`HANDOVER.md`](./HANDOVER.md) | **交接文档**：当前功能/接线/API/MQTT/NVS、已验证项、已知问题、排障与下一步 |
| `partitions.csv` | 分区表（双 OTA：`ota_0`/`ota_1` 各 3MB） |

> 约定：**每次用户目视确认「OK」后，把该状态下实际正确的参数写入 §11**（含确认日期）。
>
> **构建规则：** 每次大修改都必须按 [`BUILD_FLOW.md`](./BUILD_FLOW.md) 完成“编译 → 烧录/OTA → 真机验证 → 记录证据 → 文档同步”；用户确认 OK 后立即更新 §11。

## 1. 项目简介

工业/楼宇设备数据采集上云，同时本地屏显示状态。本工程按 **方案 A** 落地：

- **上行双链路**：W5500 有线以太网（主，esp_eth + SPI）+ Wi-Fi（备，STA），由 `net_manager` 自动切换默认路由
- **本地看板**：1.77″ TFT（后续里程碑接入 LVGL）
- **本地存储**：TF 卡（后续里程碑接入 FatFS）
- **上云**：MQTT（后续里程碑接入 `esp-mqtt`）

### 本次里程碑交付（网络骨架）

- [x] 标准 ESP-IDF 工程结构（CMakeLists / main / components / sdkconfig.defaults / README / .gitignore）
- [x] `components/board`：集中引脚定义 `board_pins.h`（W5500 / LCD / TF / UART 全部）
- [x] `components/net_eth`：W5500（esp_eth + SPI）初始化与事件处理，DHCP / 静态 IP 双模式
- [x] `components/net_wifi`：WiFi Station 初始化（DHCP、断线重连）
- [x] `components/net_manager`：双网优先级 / 切换框架（接口已就绪，轮询决策已可运行）
- [x] `README.md`：环境安装与编译步骤（本机已装好，见 §4.5）
- [x] **编译验证**：ESP-IDF v5.5.5 `idf.py build` 通过（BUILD_EXIT=0，详情见 §4.5）
- [x] **真机验证**：COM3 烧录跑通，双网骨架运行稳定（实测记录见 §5）
- [x] **里程碑 B：ST7735S + LVGL 本地看板**（驱动/验证记录见 §6）
- [x] **里程碑 C：Home Assistant MQTT 接入**（组件/协议/验证记录见 §7）

### 已决策落地（详见 §9）

- [x] **PSRAM**：外置 ESP-PSRAM64H（8MB Quad）确认并已启用（sdkconfig.defaults）
- [x] **MQTT Broker**：确认 broker.emqx.io:1883（PINMAP §7 已同步）
- [x] **WiFi 静态 IP**：已增加可选配置（默认 DHCP）

## 2. 目录结构

```
ESP32-S3-Gateway/
├── CMakeLists.txt                  # 顶层：注册 components/ 为组件目录，工程名 esp32s3_gateway
├── sdkconfig.defaults              # 默认配置：esp32s3 / 16MB Flash / SPI 以太网 / PSRAM(Quad) / 主机名 / LVGL
├── partitions.csv                  # 自定义分区表：factory app 3MB（LVGL 后 1MB 放不下）
├── .gitignore                      # 忽略 build、sdkconfig 等构建产物
├── BUILD_FLOW.md                   # 项目构建流水：编译/烧录/验证/文档同步规则
├── build/project_flow.bat          # Windows 统一构建、烧录、监视脚本
├── PINMAP.md                       # 参考：引脚映射（原理图逐脚核对）
├── SOLUTIONS.md                    # 参考：方案选型
├── main/
│   ├── CMakeLists.txt              # REQUIRES 网络 + 看板组件
│   ├── idf_component.yml           # 依赖：lvgl/lvgl v9 + espressif/esp_lvgl_port（自动下载）
│   └── app_main.c                  # NVS → 事件循环 → net_eth → net_wifi → net_manager
│                                   #   + ui_dashboard 看板 + 状态演示任务
└── components/
    ├── board/                      # 纯头文件组件：全工程唯一引脚来源
    │   ├── CMakeLists.txt
    │   └── include/board_pins.h    # W5500(10~15) / LCD(15~18,21) / TF(33~38) / UART0(43,44)
    ├── net_eth/                    # W5500 有线以太网
    │   ├── CMakeLists.txt
    │   ├── Kconfig                 # 静态 IP / SPI 时钟等菜单项
    │   ├── include/net_eth.h       # 对外接口（init / 状态查询 / netif）
    │   └── net_eth.c               # SPI 总线→MAC/PHY→netif→驱动→事件（DHCP/静态IP）
    ├── net_wifi/                   # WiFi Station：DHCP/可选静态IP、断线按次数重连
    │   ├── CMakeLists.txt
    │   ├── Kconfig                 # SSID / 密码 / 最大重连次数
    │   ├── include/net_wifi.h
    │   └── net_wifi.c              # netif → esp_wifi_init → 事件（连接/重连/获IP）
    ├── net_manager/                # 双网冗余管理器
    │   ├── CMakeLists.txt
    │   ├── include/net_manager.h   # 接口：init / get_status / register_listener / backend_name
    │   └── net_manager.c           # 3s 轮询：ETH 优先选路，esp_netif_set_default_netif 切换
    ├── ha_mqtt/                    # Home Assistant MQTT 接入（esp-mqtt 封装：通知/状态/Discovery）
    │   ├── CMakeLists.txt
    │   ├── Kconfig                 # broker / 主题 / 心跳 / 通知显示时长
    │   ├── include/ha_mqtt.h
    │   └── ha_mqtt.c               # 订阅 notify→队列投递UI；state 心跳；homeassistant/ Discovery
    ├── lcd_st7735/                 # ST7735S 1.77" TFT 驱动（自写 esp_lcd 面板，SPI3 独立总线）
    │   ├── CMakeLists.txt
    │   ├── Kconfig                 # 偏移 / 色彩顺序 / 反转 / SPI 时钟
    │   ├── include/lcd_st7735.h
    │   └── lcd_st7735.c            # SPI3 总线→面板IO→自定义面板（命令序列/描点/方向）
    └── ui_dashboard/               # LVGL 看板（lvgl + esp_lvgl_port）
        ├── CMakeLists.txt
        ├── include/ui_dashboard.h
        └── ui_dashboard.c          # 看板 UI + net_manager 监听刷新 + 运行时长任务
```

## 3. 关键文件说明

| 文件 | 说明 |
|------|------|
| `components/board/include/board_pins.h` | 全部板载引脚集中定义，外设代码一律引用，禁止自带 GPIO 编号 |
| `components/net_eth/net_eth.c` | W5500 挂在 SPI2（FSPI 默认引脚 11/12/13 + CS10），INT=14，RST=15（与 LCD_RST 共用）。DHCP 默认；`CONFIG_NET_ETH_USE_STATIC_IP=y` 启用静态 IP |
| `components/net_wifi/net_wifi.c` | STA 模式，SSID/密码在 menuconfig 配置；默认 DHCP，可选静态 IP（`CONFIG_NET_WIFI_USE_STATIC_IP`）；断线按次数自动重连，上限后暂停待上层恢复 |
| `components/net_manager/net_manager.c` | 默认优先级 **ETH > WiFi**；3 秒轮询两侧就绪状态，变化时 `esp_netif_set_default_netif()` 切换 lwIP 默认路由并广播监听器回调 |
| `components/lcd_st7735/lcd_st7735.c` | 自写 ST7735S 驱动：SPI3 独立总线、RST 恒 -1（共享复位线）、显存偏移 Kconfig 可调（详见 §6） |
| `components/ui_dashboard/ui_dashboard.c` | LVGL 看板：上行/IP/双网可用/运行时长 + HA 通知 banner；net_manager 监听即时刷新 + 每秒周期刷新（详见 §6/§7） |
| `components/ha_mqtt/ha_mqtt.c` | esp-mqtt 封装：订阅 ha/esp32gw/notify → 队列投递看板；发布 state 心跳；MQTT Discovery（详见 §7） |

### 联网行为（预期日志形态）

```
I net_eth   : 网线已插入，准备获取 IP...
I net_eth   : 以太网已获得 IP：192.168.x.x 掩码：255.255.255.0 网关：192.168.x.1
I net_wifi  : WiFi 获得 IP：192.168.x.x
I net_manager: 上行链路切换：None -> ETH（IP=192.168.x.x）   ← 有线优先
W net_eth   : 网线断开（上行切换由 net_manager 决策）
I net_manager: 上行链路切换：ETH -> WiFi（IP=192.168.x.x）   ← 自动兜底
I net_eth   : 网线已插入...
I net_manager: 上行链路切换：WiFi -> ETH（IP=192.168.x.x）   ← 有线恢复自动切回
```

## 4. 环境与编译

> ✅ 本机（Windows）已安装 ESP-IDF **v5.5.5**，工程 **编译验证通过**（见 §4.5）。
> 以下安装步骤仍保留，供其它机器 / 人员参考。

### 4.1 版本要求

- **本工程按 ESP-IDF v5.5 API 编写，已在 v5.5.5 编译验证**（建议 ≥ 5.5）
- 更低版本（5.2/5.3）需按 §8 反向调整 API 差异
- Python（v5.5 安装器自动创建 3.13 venv，见 §4.5）

### 4.2 安装方式（Windows，二选一）

**方式 A：官方 IDF 安装器（推荐，简单）**

1. 从 <https://dl.espressif.com/dl/esp-idf/> 下载 Windows 安装器（Espressif-IDE 或 IDF Tools）
2. 运行安装器，版本选 **v5.5.x**，勾选 **ESP32-S3** 目标
3. 安装完成后会用 **ESP-IDF PowerShell（或 CMD）** 快捷方式启动命令行

**方式 B：Git 克隆（熟悉 Git 者用）**

```powershell
# 本机实际位置：H:\esp\esp-idf，工具链目录 IDF_TOOLS_PATH=H:\espressif
git clone -b v5.5 --recursive https://github.com/espressif/esp-idf.git H:\esp\esp-idf
cd H:\esp\esp-idf
.\install.ps1 esp32s3        # 首次安装工具链（需联网，较久）
.\export.ps1                 # 每次打开新终端需执行：加载 idf.py 环境
```

> 若用 CMD，则使用 `install.bat` / `export.bat`。

### 4.3 编译与烧录（在 ESP-IDF 命令行中）

> 本机实测命令（Windows CMD）：
> ```bat
> cd /d H:\esp\esp-idf && call export.bat
> cd /d H:\VSCODE\ESP32-S3-Gateway && idf.py build
> ```

```powershell
cd H:\VSCODE\ESP32-S3-Gateway

idf.py set-target esp32s3     # 首次必须；会生成 sdkconfig
idf.py menuconfig             # 配置 WiFi SSID/密码、以太网静态 IP（见 4.4）
idf.py build                  # 编译
idf.py -p COMx flash monitor  # 烧录 + 打开串口日志（COMx 为 CH340K 的串口号，如 COM3）
```

常用：`Ctrl+]` 退出 monitor；`idf.py monitor` 单独打开日志。

### 4.4 必须的配置项（首次烧录前）

| 菜单路径 | 配置项 | 说明 |
|---------|--------|------|
| Component config → net_wifi | `NET_WIFI_SSID` / `NET_WIFI_PASSWORD` | **必改**，填本机 2.4G 热点 |
| Component config → net_wifi | `NET_WIFI_USE_STATIC_IP` 等 | 可选；默认 DHCP，企业网固定 IP 场景按需开启 |
| Component config → net_eth | `NET_ETH_USE_STATIC_IP` 等 | 默认 DHCP，不用改 |
| Component config → net_eth | `NET_ETH_SPI_CLK_HZ` | 默认 10MHz；稳定后可试着提高 |

也可直接编辑 `sdkconfig.defaults` 中的示例（已注释）再 build。

### 4.5 本机环境已就绪 · 编译验证记录（2026-09-24）

| 项目 | 值 |
|------|----|
| ESP-IDF | **v5.5.5**（路径 `H:\esp\esp-idf`） |
| 工具链目录 | `IDF_TOOLS_PATH=H:\espressif` |
| Python | 系统 Python 3.13.15（IDF 自动创建 venv：`H:\espressif\python_env\idf5.5_py3.13_env`） |
| 编译命令（Windows CMD） | `cd /d H:\esp\esp-idf && call export.bat` 后 `cd /d H:\VSCODE\ESP32-S3-Gateway && idf.py build` |
| 结果 | **BUILD_EXIT=0，编译通过** |

固件体积（首次编译实测）：

| 产物 | 大小 | 备注 |
|------|------|------|
| application bin | **801,872 B（0xC3C50）** | 分区表 1MB，**余 24%** |
| bootloader | **0x5160B** | 分区余量约 36% |

> 编译过程中按 v5.5 实际 API 形态修正了源码（组件名、W5500 头文件、驱动安装、DNS 等），
> 差异清单见 §8（各项均已标注「已在 IDF v5.5.5 验证」）。

### 4.6 项目构建流水（必读）

完整流程、统一脚本和注意事项见 [`BUILD_FLOW.md`](./BUILD_FLOW.md)。常用命令：

```bat
cd /d H:\VSCODE\ESP32-S3-Gateway
build\project_flow.bat reconfigure
build\project_flow.bat build
build\project_flow.bat flash COM3
build\project_flow.bat monitor COM3
```

大修改的固定顺序是：**检查工作区 → 修改代码 → reconfigure（如配置变化）→ build → 烧录或 OTA → 真机验证 → 记录 BUILD/FLASH/运行时证据 → 更新 README 与相关设计文档**。不要随意删除 `sdkconfig`；串口不可用且 Web 正常时才使用 OTA，HTTP 服务本身异常时必须恢复串口或物理重启。



### 5.1 实测结果

板子稳定运行、**无重启**；W5500 有线以太网 DHCP 成功，实测日志：

```
I net_eth   : 以太网已获得 IP：192.168.99.26 掩码：255.255.255.0 网关：192.168.99.1
I net_manager: 上行链路切换：None -> ETH（IP=192.168.99.26）
I app_main  : 状态：上行=ETH（IP=192.168.99.26）| 有线可用=1 Wi-Fi可用=0
```

> 注：Wi-Fi 显示不可用是因为 SSID 仍为占位符 `MyWiFi`；
> 在 menuconfig（Component config → net_wifi → `NET_WIFI_SSID` / `NET_WIFI_PASSWORD`）
> 填入真实热点后即可自动兜底 / 切换（预期日志形态见 §3）。

### 5.2 真机调试修复（5 项）

真机联调中发现并修复的问题（现象 → 原因 → 修复）：

| # | 现象 | 原因 | 修复（位置） |
|---|------|------|--------------|
| 1 | 【最关键】收到以太网包即断言 **`tcpip_inpkt: Invalid mbox`**，反复重启 | LWIP 的 tcpip 邮箱未初始化（缺 `esp_netif_init()`） | `app_main` 在 `esp_event_loop_create_default()` **之前**调用 `esp_netif_init()`（main/app_main.c） |
| 2 | W5500 版本寄存器读 **0x00**（期望 0x04） | `phy_config.reset_gpio_num = -1`，驱动 install 时未做硬件复位（该 GPIO 用于 `phy->reset_hw`） | 设为 `BOARD_ETH_PIN_RST`（GPIO15，与 LCD_RST 共用）（net_eth.c） |
| 3 | 版本仍读 **0x01**（错误） | SPI 时序不匹配：`devcfg` 多加了 `cs_ena_pretrans` / `cs_ena_posttrans` | 移除这两项，与 IDF esp_eth 官方 W5500 例程一致（net_eth.c） |
| 4 | 日志报 **`gpio_isr_handler_add: GPIO isr service is not installed`** | W5500 中断（GPIO14）模式需先安装 GPIO ISR 服务 | `net_eth_init` 中、驱动 install **之前**调用 `gpio_install_isr_service(0)`（net_eth.c） |
| 5 | netif glue 打印 MAC 为 **`00:00:00:00:00:00`** | W5500 无出厂 MAC | install 后 `esp_read_mac(mac, ESP_MAC_ETH)` + `esp_eth_ioctl(s_eth, ETH_CMD_S_MAC_ADDR, mac)` 设置（实测生效 `44:1b:f6:ec:c5:8f`）（net_eth.c） |

> 说明：以上为**运行期**问题；编译期 API 差异见 §8。

## 6. 里程碑 B：ST7735S + LVGL 本地看板（已完成）

1.77″ TFT 显示双网状态看板：当前上行 / IP / 有线与 Wi-Fi 可用性 / 运行时长；
数据实时来自 `net_manager`（链路切换监听器即时刷新 + 每秒周期刷新）。

### 6.1 硬件与接线（ST7735S，128x160）

| 信号 | GPIO | 说明 |
|------|------|------|
| MOSI | 17 | SPI 数据 |
| SCLK | 18 | SPI 时钟 |
| CS | 21 | 片选 |
| D/C | 16 | 数据/命令 |
| RST | 15 | **与 W5500_RST 共用**（驱动不操作该引脚） |

- LCD 独立挂 **SPI3_HOST**，避开 W5500 独占的 SPI2（FSPI 默认引脚 10~13），两总线互不干扰。

### 6.2 驱动与组件说明

- **`components/lcd_st7735`（自写 ST7735S 驱动）**
  - 基于 `esp_lcd_panel_io_spi` + 自定义 `esp_lcd_panel_t` 接口（初始化命令序列 / 描点 / 方向控制）
  - **共享复位线约束**：`reset_gpio_num` 恒为 -1；`reset()` 退化为软件复位（SWRESET 经 SPI 下发，
    不触碰 GPIO15，不会复位 W5500）；LCD 初始化在 `net_eth_init()` 之后（app_main 顺序已保证）
  - ST7735S 显存 132x162 vs 面板 128x160：**本板实测 OFFSET=(0,0)**（非 0 会导致
    窗口回卷斜切，根因与排查见 §6.7）；旋转/偏移体系见 §6.6 校准表：
    `LCD_ST7735_ROTATION_*`(0/90/180/270) / `LCD_ST7735_MIRROR_X/Y` /
    `LCD_ST7735_OFFSET_X/Y` / `LCD_ST7735_OFFSET_MANUAL` /
    `LCD_ST7735_RGB_BGR` / `LCD_ST7735_INVERT_COLOR` / `LCD_ST7735_SPI_CLK_HZ` /
    `LCD_ST7735_H_RES` / `LCD_ST7735_V_RES`
- **`components/ui_dashboard`（LVGL v9 看板）**
  - LVGL 9.6 + `espressif/esp_lvgl_port` 由 Component Manager 自动拉取（`main/idf_component.yml` 声明）
  - 单缓冲整帧 40KB（DMA）；`swap_bytes` 处理 ST7735S 16bpp 大端字节序；全屏刷新
  - 线程安全：所有 UI 更新持 `lvgl_port_lock`；“先取网络快照再加锁刷 UI”，避免与
    net_manager 监听器回调形成锁序死锁（ABBA）
  - 屏显使用拉丁字符（LVGL 内置 Montserrat 无中文；中文界面需嵌入中文字体，后续可加）

### 6.3 看板内容

| 行 | 内容 |
|----|------|
| 标题 | NET GW |
| 上行 | Uplink: ETH（绿）/ WIFI（橙）/ NONE（红） |
| IP | 当前上行 IP（如 192.168.99.26） |
| 有线 | Eth OK / OFF |
| Wi-Fi | WiFi OK / -- |
| 底部 | 运行时长 Up hh:mm:ss |

> 布局（128x160 全屏排布，防溢出）：标题 14px 顶部居中；正文统一 **12px**、
> 行距 18px、左右边距 6px；每个信息行限宽(116px) + WRAP 换行，防止极端长文本
> （如 IP）溢出裁切；运行时长底对齐。曾因字号偏大 / Box 嵌套重叠导致文字溢出，
> 见 §6.8-d。

### 6.4 分区调整

LVGL 使 app 超过默认 1MB 分区（当前 ~1.15MB），改用自定义分区表 `partitions.csv`：
`factory` app 分区扩到 **3MB**（16MB Flash 充裕）；NVS 24KB、phy_init 4KB 保持默认区域；
后续若做 OTA 再改双分区。

### 6.5 真机验证结果（2026-09-25，COM3）

- 编译：BUILD_EXIT=0；app 二进制 **0x11FDC0（~1.15MB）**，3MB 分区余 63%
- 烧录：FLASH_EXIT=0（bootloader + 分区表 + app 均校验通过）
- 串口日志（节选，**初版 OFFSET=(2,1) 配置**；该配置后被 §6.7 判定为斜切根因，
  本板实际应使用 (0,0)）：

```
I lcd_st7735  : ST7735S 面板创建成功：SPI3, 40000000 Hz, 偏移(x=2,y=1), BGR=1
I lcd_st7735  : ST7735S 初始化完成：madctl=0x08, invert=0
I LVGL        : Starting LVGL task
I ui_dashboard: LVGL 显示设备挂接成功（128x160 RGB565，40KB 缓冲）
I ui_dashboard: 本地看板启动完成（数据来自 net_manager）
I net_eth     : 以太网已获得 IP：192.168.99.26 掩码：255.255.255.0 网关：192.168.99.1
I net_manager : 上行链路切换：None -> ETH（IP=192.168.99.26）
```

- 结论：**LCD 初始化成功、LVGL 看板运行稳定（25s 观察窗口无 panic / 无 watchdog 重启）、
  DHCP 正常获 IP 并经看板显示**；既有双网功能（ETH 优先 + 切换）完全不受影响
- 若屏幕方向/偏移/颜色异常，按 §6.6 调整对应 Kconfig 项即可，无需改代码

### 6.6 显示校准（旋转 / 镜像 / 偏移 / 测试图）

ST7735S 显存 132x162，面板 128x160；旋转由 MADCTL 位实现，且旋转会改变
CASET/RASET 窗口的偏移定义。驱动在创建面板时按查表自动换算（可通过
`LCD_ST7735_OFFSET_MANUAL` 关闭自动换算、完全手工指定偏移）：

| 旋转 | MADCTL 基础位 | 生效偏移 (x,y) | 分辨率 | 说明 |
|------|---------------|----------------|--------|------|
| 0°   | 0x00          | (0,0)         | 128x160 | 竖屏基准 |
| 90°  | MV\|MX (0x60) | (0,0)         | 160x128 | 列/行偏移互换（0/0 时无变化） |
| 180° | MX\|MY (0xC0) | (0,0)         | 128x160 | 偏移不变（同窗口反向扫描） |
| 270° | MV\|MY (0xA0) | (0,0)         | 160x128 | 列/行偏移互换（0/0 时无变化） |

> 生效偏移 = 基准 OFFSET_X/Y 按旋转查表示例交换所得，基准默认 **(0,0)**（本板实测）。
> 实际 MADCTL = 上表基础位 ± MIRROR_X(MX) / MIRROR_Y(MY) + BGR(0x08)。
> 日志会打印当前生效值（本板实测），例如：`ROTATION=180 MADCTL=0xC0 OFFSET=(0,0) RES=128x160`。

**测试图诊断模式**（`CONFIG_LCD_ST7735_TEST_PATTERN=y`）：绕过 LVGL，直接用
esp_lcd 面板接口绘制静态诊断图 —— 1px 白色边框（看裁切/偏移）+ 四角 16x16
色块 · 左上红/右上绿/左下蓝/右下黄（看旋转/镜像方向）+ 全屏红/绿/蓝三条竖色带
（各约 1/3 宽，看扫描是否笔直、有无斜切）。日志会打印
`测试图：整帧 128x160 = 40960 B` 等关键信息。

**调整方法**（以改为 180° 为例，同理可调 MIRROR_X/Y、OFFSET_X/Y 等）：

```
# 编辑 sdkconfig（build 时自动生效；或 idf.py menuconfig 菜单选择）
# CONFIG_LCD_ST7735_ROTATION_0 is not set
CONFIG_LCD_ST7735_ROTATION_180=y
# 诊断/正常模式切换：
# CONFIG_LCD_ST7735_TEST_PATTERN=y   （y=测试图诊断；n=LVGL 看板）
```

当前状态：`ROTATION=180 MIRROR_X=0 MIRROR_Y=0 MADCTL=0xC0 OFFSET=(0,0) RES=128x160 TEST_PATTERN=n`
（已全部调通：方向 180°、颜色 RGB；四步调通过程见 §6.8，斜切修复细节见 §6.7）

### 6.7 显示异常排查与修复：斜切（45° 斜条 + 白色边框回卷）

> 本板 1.77" ST7735S 在里程碑 B 初版显示异常，经诊断确认为 CASET/RASET 窗口越界回卷，
> 修复后验证正常。本节记录现象 / 诊断方法 / 根因 / 修复，供同类 ST7735 面板复用。

**① 现象**

- 本应竖直的红/绿/蓝三条色带变成约 **45° 斜条**
- **白色边框重复出现**（同一条边框在屏内多次出现）—— 说明内容在回卷

**② 诊断过程与方法（可复用）**

- 让用户手机拍屏 → 用 **Python（PIL + numpy）分析照片**：
  - 检测 LCD 玻璃左右边缘斜率 ≈ **-1.4°**（几乎竖直）→ 排除“拍摄角度造成的假斜切”
  - 读出色带确实倾斜 → 判定为**面板内真实剪切**
  - 斜向白线按周期重复 → 判定为**内容回卷** → 根因锁定在
    「CASET/RASET 窗口与面板实际行宽不符」

**③ 根因**

- 原 OFFSET=(2,1) 依据“显存 132-面板 128、显存 162-面板 160”的常规推测得出；
  **本板面板实际行宽不含该余量**。窗口被设为 (2..129)，超过面板真实列宽，
  每行末尾回卷到下一行 → 呈现斜切。

**④ 修复**

- `CONFIG_LCD_ST7735_OFFSET_X=0`、`CONFIG_LCD_ST7735_OFFSET_Y=0`
  （Kconfig 默认值已由 2/1 改为 **0/0**，help 中写明“非 0 会导致斜切”；
  `lcd_st7735.h` 注释同步为「本板实测 =0；非 0 会导致斜切」）

**⑤ 最终生效配置（实测正常）**

> ⚠️ 本节为「斜切修复」当时（ROTATION=0 / BGR）的历史记录；
> 后续又经 §6.8 的 b/c 步骤校准了方向（180°）与颜色（RGB），
> **当前最终配置以 §6.8 总结为准**。

```
ROTATION=0  MIRROR_X=0  MIRROR_Y=0  MADCTL=0x08(BGR)  OFFSET=(0,0)  RES=128x160
LCD: SPI3_HOST @40MHz；TEST_PATTERN=n（恢复正常 LVGL 看板）
```

- 串口：
  `lcd_st7735: ST7735S 面板创建成功：SPI3, 40000000 Hz | ROTATION=0 MIRROR_X=0 MIRROR_Y=0 MADCTL=0x08 OFFSET=(0,0) RES=128x160`
- 看板恢复：`LVGL: Starting LVGL task` / `ui_dashboard: LVGL 显示设备挂接成功（128x160 RGB565，40KB 缓冲）`
- 双网不受影响：ETH 仍正常获 IP 192.168.99.26

**⑥ 诊断工具用法（校准面板用）**

- `CONFIG_LCD_ST7735_TEST_PATTERN=y`：绕过 LVGL 绘制静态诊断图（四角 16x16 色块
  [左上红/右上绿/左下蓝/右下黄] + 1px 白色边框 + 全屏红/绿/蓝三色竖带），
  用于校准 ROTATION / OFFSET / 裁切；校准完成后设为 n 恢复看板
- `CONFIG_LCD_ST7735_ROTATION`(0/90/180/270)、`MIRROR_X/Y`、`OFFSET_X/Y`、
  `RGB_BGR`、`INVERT_COLOR`、`OFFSET_MANUAL`：方向/偏移/颜色调整
- 修改 `sdkconfig` 后执行 `cmd //c "H:\esp\do_bf.bat"` 即生效（无需删缓存）

### 6.8 LCD 调通四步（最终结论，2026-09-25）

> 从“画面异常”到完全调通共经历四类问题，均在本板实机验证。这是本次最有价值的沉淀，
> 同类 ST7735 面板可直接对号入座（斜切详细过程见 §6.7）。

| 步骤 | 现象 | 根因 | 修复 |
|------|------|------|------|
| **a 斜切** | 竖直红/绿/蓝三色带变 45° 斜条；白色边框回卷重复 | `OFFSET=(2,1)` 使 CASET 窗口超出面板实际行宽 → 每行末尾回卷 | `OFFSET_X/Y=0`（详见 §6.7） |
| **b 上下颠倒**（极具迷惑性） | **测试图正常、但 LVGL 看板上下颠倒** | esp_lvgl_port 在 `lvgl_port_add_disp()` 时会调用 `esp_lcd_panel_mirror(false,false)`（及 swap_xy）；本驱动 mirror()/swap_xy() 会**重写 MADCTL**，把面板初始化时设好的 ROTATION_180（MX\|MY=0xC0）覆盖回 0x00 | ui_dashboard 的 `lvgl_port_display_cfg_t.rotation` **跟随 lcd_st7735 的 Kconfig**：`ROTATION_180 → mirror_x=true, mirror_y=true`（幂等，保持 0xC0）；`90/270 → swap_xy=true`（及镜像组合） |
| **c 红蓝互换** | 右下角黄色方块显示成**青色**（决定性证据） | MADCTL 的 BGR 位用反 | `CONFIG_LCD_ST7735_RGB_BGR=n`（即 RGB） |
| **d 布局溢出** | 文字超出 128x160、被裁切 | 字号偏大（14/16px）+ Box 嵌套与底部运行时长重叠 | 正文缩至 12px、行距 18px、左右边距 6px、行限宽 + WRAP 防溢出（§6.3） |

**诊断方法（可复用）**

- `LCD_ST7735_TEST_PATTERN=y` 静态测试图：四角 16x16 色块（左上红/右上绿/左下蓝/右下黄）+
  1px 白边框 + 全屏红/绿/蓝三色竖带，单独判定方向 / 偏移 / 裁切 / 颜色
- 用户手机拍屏 → **Python(PIL + numpy) 分析照片**：
  - 检测 LCD 玻璃边缘倾角标定，排除“拍摄角度造成的假斜切”
  - 出现设计中没有的颜色（如右下黄块变青）→ 直接判定**颜色通道互换**（BGR 位反）

**最终生效配置（实测正常，请勿再改）**

```
CONFIG_LCD_ST7735_ROTATION_180=y        # 方向
CONFIG_LCD_ST7735_MIRROR_X / MIRROR_Y = n
CONFIG_LCD_ST7735_OFFSET_X = 0  /  OFFSET_Y = 0
CONFIG_LCD_ST7735_RGB_BGR = n           # 即 RGB（BGR 位关）
CONFIG_LCD_ST7735_SPI_CLK_HZ = 40000000；RES = 128x160；TEST_PATTERN = n
串口：ROTATION=180 MIRROR_X=0 MIRROR_Y=0 MADCTL=0xC0 OFFSET=(0,0) RES=128x160
```

看板布局：正文 12px、行距 18px、标题 14px，全部内容完整落在 128x160 内（§6.3）。

## 7. 里程碑 C：Home Assistant MQTT 接入（已完成）

网关接入本地 Home Assistant：接收 HA 推送通知显示到 LCD 看板，并周期上报状态
（uplink / ip / uptime），配合 MQTT Discovery 让 HA 自动生成实体。

### 7.1 组件与架构

- **`components/ha_mqtt`**：esp-mqtt（IDF 内置子模块组件 `mqtt`，无需托管下载）封装
  - 连接 broker（Kconfig 可配 host/port/user/pass/client_id，默认匿名）
  - 订阅 `ha/esp32gw/notify`：解析 JSON 通知 → **FreeRTOS 队列投递**给看板
    （MQTT 回调只解析+入队，【绝不在回调里操作 LVGL】，避免阻塞 MQTT 任务）
  - 发布 `ha/esp32gw/state`：心跳 5s 周期 + 收到通知立即补发
  - MQTT Discovery：向 `homeassistant/sensor/esp32gw_*` 发布配置自动生成实体
  - 断线重连由 esp-mqtt 自动处理；连接使用默认 netif（ETH 优先，net_manager 已切换）
- **`components/ui_dashboard`**：新增 HA 通知 banner（覆盖 KPI 区 y=24..134，不碰标题/运行时长），
  按 level 着色（info 蓝 / warn 橙 / error 红），`CONFIG_HA_MQTT_NOTIFY_SHOW_MS`（默认 10s）后
  自动恢复常规状态；handler 由 ha_mqtt 的投递任务调用（内部 lvgl_port_lock），
  符合“不在 MQTT 回调直接操作 LVGL”约定。

### 7.2 配置（menuconfig → Component config → ha_mqtt）

| Kconfig | 默认 | 说明 |
|---------|------|------|
| `HA_MQTT_BROKER_HOST` | `192.168.99.253` | 本地 HA broker（已实测匿名可用） |
| `HA_MQTT_PORT` | `1883` | |
| `HA_MQTT_USER` / `HA_MQTT_PASSWORD` | 空（匿名） | |
| `HA_MQTT_CLIENT_ID` | 空（自动生成） | |
| `HA_MQTT_TOPIC_NOTIFY` | `ha/esp32gw/notify` | HA 推送主题（JSON 通知） |
| `HA_MQTT_TOPIC_STATE` | `ha/esp32gw/state` | 状态主题 |
| `HA_MQTT_HEARTBEAT_MS` | `5000` | 状态心跳周期 |
| `HA_MQTT_DISCOVERY_ENABLE` | `y` | MQTT Discovery（HA 自动发现实体） |
| `HA_MQTT_NOTIFY_SHOW_MS` | `10000` | 看板通知显示时长（超时恢复常规状态） |

### 7.3 消息协议

- **notify（HA → 板）**：`{"title":"...","message":"...","level":"info|warn|error"}`
- **state（板 → HA，5s 心跳 + 通知后补发）**：
  `{"online":true,"uplink":"ETH","ip":"192.168.99.26","uptime":55,"notify_title":"...","notify_message":"..."}`
- **Discovery**：`homeassistant/sensor/esp32gw_{uplink,ip,uptime,notify}/config`，
  value_template 取 state JSON 字段，availability 由 `online` 字段驱动

### 7.4 真机验证（2026-09-25，COM3）

- 编译/烧录：BUILD_EXIT=0 / FLASH_EXIT=0；app 0x1468D0（3MB 分区余 57%）
- MQTT 连接：启动时 ETH 尚未获 IP → `Error transport connect` → **esp-mqtt 自动重连** →
  以太网 DHCP 完成后连上，并完成订阅 + Discovery×4 发布
- PC 端 paho-mqtt 验证：
  - 发布 notify `{"title":"Alarm","message":"Front door opened","level":"error"}` →
    板子串口 `ha_mqtt: 收到通知：[Alarm] Front door opened`；
    `ui_dashboard: 通知显示已到期，恢复常规状态`（10s 显示窗口机制生效）
  - 订阅 `ha/esp32gw/state`：持续收到心跳（uptime 递增），且 notify 后**立即补发**（含 notify_title 字段）
  - MQTT 连接前置条件：ETH 先在 6.5s 获 IP 192.168.99.26，broker 同网段可达
- 双网/LCD 功能不受影响：上行切换、看板、DHCP 均正常
- **待用户目视确认**：通知显示期 LCD banner 效果（颜色 / 换行 / 10s 后恢复）

## 8. 已知问题与 API 兼容性说明（已在 IDF v5.5.5 验证）
> 以下 API 差异均为 **IDF v5.5.5 实际编译修好** 的结论（2026-09-24），源码已按此形态编写，后续如换 IDF 版本请对照调整。

**编译修复的 API 差异（v5.5 实测）**

1. **组件依赖名 `esp_log` → `log`**：IDF v5.5 中日志组件已更名为 `log`，
   涉及 `net_eth` / `net_wifi` / `net_manager` / `main` 四个 CMakeLists 的 REQUIRES。
2. **W5500 头文件**：`esp_eth_mac_w5500.h` / `esp_eth_phy_w5500.h` 已不存在，
   改用 `esp_eth_mac_spi.h`（声明 `esp_eth_mac_new_w5500`）与 `esp_eth_phy.h`
   （声明 `esp_eth_phy_new_w5500`）；netif glue 声明在 `esp_eth_netif_glue.h`。
3. **`esp_eth_mac_new_w5500` 参数顺序**：v5.5 签名为 `esp_eth_mac_new_w5500(&w5500_config, &mac_config)`，
   与早期写法（mac 在前）**相反**；且 `eth_w5500_config_t` 字段为 `int_gpio_num` / `poll_period_ms` /
   `spi_host_id` / `spi_devcfg` —— `spi_devcfg` 是指针 `spi_device_interface_config_t *`，
   原 `spi_config` 字段已不存在。
4. **驱动安装（v5.5 标准 API）**：`esp_eth_driver_config_t` / `esp_eth_driver_new` 不存在，
   改为 `esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy)` +
   `esp_eth_driver_install(&eth_config, &s_eth)`，随后依次
   `esp_eth_new_netif_glue` + `esp_netif_attach` + `esp_eth_start`。
5. **DNS 设置**：`esp_netif_dns_setserver` 已移除，改用
   `esp_netif_set_dns_info(netif, ESP_NETIF_DNS_MAIN, &dns)`，其中 `dns` 为
   `esp_netif_dns_info_t`（内含 `esp_ip_addr_t ip`）。
6. **`ESP_RETURN_ON_ERROR`**：需 `#include "esp_check.h"`（net_eth.c / net_wifi.c 已补）。

**硬件注意（与编译无关，保留）**

- **RST 共用**：GPIO15 同时是 W5500_RST 与 LCD_RST。当前由 net_eth 持有复位；
  后续 LCD 里程碑接入时，LCD 组件不得重复操作该引脚作输出。
- **INT 中断**：GPIO14 已配置为 W5500 中断输入；若外接设备误占需移除。

## 9. 待决策点 / 待办（下个里程碑再处理）

| # | 事项 | 说明 |
|---|------|------|
| 1 | ~~PSRAM 启用方式~~ **已决策** ✅ | 核实为**外置 ESP-PSRAM64H（8MB，Quad 四线）**，挂 SPI0_CS1 走 GPIO26~32，**不占用 GPIO33~38**，与 TF 卡无冲突。已在 `sdkconfig.defaults` 启用（`CONFIG_SPIRAM_MODE_QUAD`）。⚠️ 注意：S3R8/S3R8V 模块内置 PSRAM 是八线 OCT，勿把 OCT 配置误用到本板 |
| 2 | ~~MQTT Broker 地址~~ **已决策** ✅ | 实测仅 **broker.emqx.io:1883** 可达（匿名）；54.244.173.190 与 137.135.83.217 均超时弃用，上云统一用它（PINMAP §7 已同步） |
| 3 | ~~WiFi 静态 IP~~ **已实现** ✅ | `net_wifi` 已仿照 net_eth 增加可选静态 IP：`CONFIG_NET_WIFI_USE_STATIC_IP` / `_STATIC_IP` / `_NETMASK` / `_GATEWAY` / `_DNS`，默认关闭（DHCP） |
| 4 | **切换策略细节** | net_manager 目前是 3s 轮询 + 立即切换；后续可加滞回去抖、DNS/ARP 探活、策略可配置 |
| 5 | **UART0 日志** | 引脚 43/44 已在 board_pins.h 定义，若日后把 UART0 让给业务串口（做串口服务器方案 G），日志需改走 USB JTAG/CDC |

## 10. 后续里程碑路线（参照 SOLUTIONS.md §4）

1. **LCD 看板**：ST7735S（GPIO Matrix 到 SPI2/SPI3）+ LVGL，显示联网状态、当前上行链路、IP
2. **MQTT 上下行**：`esp-mqtt`（通过 Component Manager，需在 `main/idf_component.yml` 声明依赖），
   Broker 已确认：**broker.emqx.io:1883**（匿名接入）；
   订阅/发布主题，断网数据缓存 + 补传（接 net_manager 监听器）
3. **TF 记录**：`esp_vfs_fat` + `sdmmc`（Slot 0 默认引脚），历史数据 / 日报落盘
4. **业务层**：数据采集 / 控制逻辑，联调 PSRAM 大缓冲（PSRAM 已启用，配置见 sdkconfig.defaults；决策记录见 §9 待办 1）

---

## 11. 已确认参数记录（用户实测 OK）

> **工作流**：每次用户目视确认「OK」后，把该状态下**实际正确**的参数更新到本表（含确认日期），
> 保证文档与实机一致。这是本项目约定的固定流程。

### 11.1 LCD / 看板 —— 2026-09-25 ✅ 用户确认正常

| 项目 | 正确值 | 说明 / 踩坑 |
|------|--------|------------|
| 旋转 | `CONFIG_LCD_ST7735_ROTATION_180=y` | 面板原生方向与预期差 180° |
| MADCTL | `0xC0`（MX\|MY，BGR 位关） | 由旋转+Kconfig 推出，日志可核对 |
| 颜色顺序 | `CONFIG_LCD_ST7735_RGB_BGR=n`（**RGB**） | 若设 BGR 则**红蓝互换**（黄→青） |
| 偏移 | `CONFIG_LCD_ST7735_OFFSET_X=0`、`_Y=0` | 非 0 会使窗口超出面板行宽 → **斜切回卷** |
| SPI | `SPI3_HOST` @ `40000000` Hz | 与 W5500 的 SPI2 物理隔离 |
| 分辨率 | `128x160` | |
| 测试图 | `CONFIG_LCD_ST7735_TEST_PATTERN=n` | `y`=诊断图；`n`=正常 LVGL 看板 |
| 看板字体 | 标题 14px / 正文 12px（`lv_font_montserrat_*`） | 字号偏大会导致文字溢出 |
| 看板布局 | 内容区宽 116px、行间距 18px、全屏直接排布 | 防溢出/重叠 |
| 看板方向 | `ui_dashboard` 的 `rotation` **必须跟随** `LCD_ST7735_ROTATION_*` | `esp_lvgl_port` 会调 `esp_lcd_panel_mirror()` **重写 MADCTL**，不一致会让看板方向回退（测试图却正常） |

**串口核对串**：`lcd_st7735: ST7735S 面板创建成功：SPI3, 40000000 Hz | ROTATION=180 MIRROR_X=0 MIRROR_Y=0 MADCTL=0xC0 OFFSET=(0,0) RES=128x160`

### 11.2 网络 / 存储 / 系统 —— 2026-09-24 ✅ 已实测

| 项目 | 正确值 | 说明 |
|------|--------|------|
| 以太网 | W5500 + DHCP，实测获 IP `192.168.99.26`（网关 192.168.99.1） | |
| 板卡 MAC | `esp_read_mac(ESP_MAC_ETH)` → 实测 `44:1b:f6:ec:c5:8f` | W5500 无出厂 MAC，不设会全 0 |
| 上行切换 | ETH 优先 > WiFi 兜底，3s 轮询 | 拔/插网线可观察自动切换 |
| MQTT Broker | `broker.emqx.io:1883`（匿名） | 实测可达；另两地址超时弃用 |
| PSRAM | 外置 **ESP-PSRAM64H** 8MB，Quad，@80MHz | 实测 `Found 8MB PSRAM device` |
| Flash | 16MB | |
| 分区 | 自定义 `partitions.csv`，app = 3MB | 原 1MB 不够 LVGL |
| WiFi | 默认 DHCP；SSID 仍为占位符 `MyWiFi`（待填真实热点） | 可选静态 IP 已实现，默认关 |

### 11.3 MQTT / Home Assistant / 传感器 —— 2026-09-25 ✅ 用户确认正常

| 项目 | 正确值 | 说明 |
|------|--------|------|
| MQTT Broker | `192.168.99.253:1883`（**匿名**可用） | HA 所在 PC 的 broker，已实测 |
| Home Assistant | `http://192.168.99.253:8123` | HA Web UI（其 MQTT 集成指向同一 broker） |
| 板卡地址 | ETH DHCP `192.168.99.26` | 与 broker 同网段 |
| **HA 设备名** | **`Sensor-SU-1`**（`CONFIG_HA_MQTT_DEVICE_NAME`） | Discovery 的 device.name，实测已生效 |
| 通知订阅主题 | `ha/esp32gw/notify` | JSON {"title","message","level":"info\|warn\|error"}；看板显示 **30s**（`CONFIG_HA_MQTT_NOTIFY_SHOW_MS=30000`） |
| 状态发布主题 | `ha/esp32gw/state` | {"online","uplink","ip","uptime","notify_title","notify_message","http"}，心跳 5s（http=最后处理的 Web 请求，用于诊断） |
| 设备开关主题 | `ha/esp32gw/dev/<id>/{set,state}`（`CONFIG_HA_MQTT_TOPIC_DEV_PREFIX`） | 开关：Discovery 生成 switch，命令 ON/OFF/1/0，state retain |
| 传感器状态主题 | `ha/esp32gw/dev/<id>/state` | JSON：`{"temp":..,"hum":..}` / `{"lux":..}` / `{"level":..}`；HA 按指标生成 sensor |
| **传感器订阅主题** | **`esp32-sensor/data`**（`CONFIG_HA_MQTT_TOPIC_SENSOR`） | JSON {"temp","hum","rssi","uptime"}，现场节点约 **15s** 一条 |
| 传感器离线判定 | **60s**（`CONFIG_HA_MQTT_SENSOR_STALE_MS`） | 超时看板显示 `温--℃ 湿--%` |
| 看板传感器行 | y=98，`温24.2℃ 湿44.5%`，`LV_LABEL_LONG_WRAP` 兜底 | CJK 字体下约 109px，116px 内容宽内不溢出（带空格会溢出—已踩坑） |
| Discovery | `homeassistant/sensor/esp32gw_{uplink,ip,uptime,notify}/config` | HA 自动生成实体（含 device / availability） |
| 断线重连 | esp-mqtt 自动重连 | ETH 未获 IP 时先失败、DHCP 完成后自动连上 |
| 公网备用 broker | `broker.emqx.io:1883`（匿名） | 连通性已测；工程实际用本地 HA broker |

### 11.4 中文字体（HA 通知显示中文必需）—— 2026-09-25 ✅ 用户确认正常

| 项目 | 正确值 | 说明 |
|------|--------|------|
| 字体符号 | `lv_font_cjk_14` | 自建，随 `components/ui_dashboard/fonts/lv_font_cjk_14.c` 一起编译 |
| 字库来源 | `C:\Windows\Fonts\simhei.ttf`（黑体） | 个人自用 OK；**对外分发建议换 OFL 字体**（思源黑体/Noto Sans SC）重新生成 |
| 覆盖字符 | **GB2312 全集 7445 字** + ASCII `0x20–0x7F` | 覆盖常用汉字与全角标点 |
| 规格 | size 14 / bpp 4 / LVGL 格式（默认 **RLE 压缩**） | |
| **LVGL 开关** | **`CONFIG_LV_USE_FONT_COMPRESSED=y`（必须！）** | 字体为压缩格式；不开则字形解码为空 → **文字完全不可见**（本项目踩过的坑） |
| 生成命令 | `lv_font_conv --font simhei.ttf --size 14 --bpp 4 --format lvgl --lv-font-name lv_font_cjk_14 --lv-include lvgl.h -r 0x20-0x7F --symbols "<GB2312表>" -o components/ui_dashboard/fonts/lv_font_cjk_14.c` | **`--lv-include lvgl.h` 必须**（默认 `lvgl/lvgl.h` 会报头文件找不到）；不加 `--no-compress` 即为压缩格式 |
| 内置 CJK 字体 | 已关闭（`CONFIG_LV_FONT_SOURCE_HAN_SANS_SC_14_CJK=n`） | LVGL 自带思源黑体字符集不全（实测缺 门/铃/按/请/灯） |
| 体积 | app ≈ **2.19MB** / 3MB 分区（余 30%） | |

### 11.5 Web 控制台 + mDNS —— 2026-09-26 ✅ 已实测

| 项目 | 正确值 | 说明 |
|------|--------|------|
| HTTP 端口 | **80**（`CONFIG_WEB_CONSOLE_PORT`） | esp_http_server，绑默认 netif（ETH/WiFi 都能访） |
| 访问地址 | **`http://esp32gw.local/`** 或 `http://192.168.99.26/` | mDNS 主机名 `esp32gw` |
| mDNS 组件 | **`espressif/mdns`（组件仓库）** | ⚠️ IDF v5.x 已把 mdns 移出内核，必须用 `idf_component.yml` 声明 |
| 鉴权 | **无 Token**（已移除） | 页面与所有 `/api/*` 可直接访问；仅限可信局域网，禁止暴露公网 |
| GPIO 白名单 | **GPIO1–9 / 39–42 / 47–48** | 板载占用（0/10–21/26–38/43–46）在 GPIO 页作为说明窗展示，不可定义 |
| 屏幕显示 | Web「设置与升级 → 屏幕显示」勾选 | 温湿度、开关状态、IP、日期时间、上行链路、ETH/WiFi、运行时长、二维码；BOOT 长按 2s 回到桌面 |
| 设备类型 | 开关 / 温湿度 / 光照 / 声音 | SHT31、SHT4x（I2C 0x44）；BH1750（I2C 0x23）；DHT22（单总线）；声音用 ADC1（mV） |
| I2C 总线 | 默认 **SDA=GPIO47 / SCL=GPIO48**（Web 可改） | 100kHz，内部上拉；SHT31/4x 0x44，BH1750 0x23；可扫描地址 |
| 模拟输入 | **仅 ADC1 = GPIO1–9** | ADC2（GPIO11–20）在 WiFi 下不可用；声音模块输出接任意 ADC1 脚 |
| 接口 | `/api/status` `/api/gpio` `/api/gpio/define` `/api/gpio/remove` `/api/gpio/bus` `/api/gpio/scan` `/api/device` `/api/screen` `/api/screen/text` `/api/screen.bmp` `/api/history` `/api/logs` `/api/config` `/api/reboot` `/api/ota` | `POST /api/gpio/define`（JSON kind/model/pin/name/desc）、`POST /api/gpio/bus`、`POST /api/gpio/scan`、`POST /api/device?id=<id>&value=0|1` |
| 温湿度曲线 | **SVG 双轴**（温度左轴/湿度右轴），240 条 ≈ 1 小时，纯前端无库 | 已取代屏幕截图功能 |
| ~~屏幕截图~~ | **已移除** | 与 `/api/status` 数据重复，日常无价值 |

### 11.6 参数配置(NVS) / OTA / 分区表 —— 2026-09-26 ✅ 已实测

| 项目 | 正确值 | 说明 |
|------|--------|------|
| 配置存储 | NVS 命名空间 **`gwcfg`** | 键：wifi_ssid / wifi_pass / mqtt_host / mqtt_port / topic_notify / topic_sensor |
| 优先级 | **NVS > Kconfig 默认** | 由 `app_cfg` 组件统一提供，net_wifi / ha_mqtt 均已改为读取它 |
| 生效方式 | 页面「**保存并重启设备**」→ `POST /api/config` + `POST /api/reboot` | 实测：保存后 20s 内恢复服务 |
| OTA | `POST /api/ota`（原始 bin，需浏览器选中 `build/esp32s3_gateway.bin`） | 实测上传 2,184,416 字节 → 切换分区 → 重启成功 |
| 分区表 | `nvs / otadata / phy_init / **ota_0(3M) / ota_1(3M)**` | 支持 A/B 双分区回滚 |
| 历史回灌 | 开机读 TF 当日 CSV 的**最近 240 条** sensor 记录 | 曲线**跨重启连续**（实测 `回灌 240 条`） |
| 实测配置 | WiFi SSID = **`BBC`**（用户已通过页面设置，`from_nvs=true`） | 双网切换实测待完成（见 §12） |

### 11.7 时间同步（TF 记录时间戳）—— 2026-09-26 ✅ 已实测

| 项目 | 正确值 | 说明 |
|------|--------|------|
| 方式① | SNTP `ntp.aliyun.com` | 需外网；本环境**不通**（PC 走代理），故失败 |
| 方式② | **HTTP `Date` 响应头校时** | 向 HA（`CONFIG_HA_MQTT_BROKER_HOST:8123`）发 `HEAD /`，解析 `Date` 头 |
| 实测 | `时间已通过 HTTP Date 校准：2026-09-26 15:46:05` | 仅需局域网，无需外网 |
| 退避 | 未校时时 TF 文件名用 `log_uptime.csv`，时间戳用 `uptime=Ns` | 校时后自动切到 `log_YYYYMMDD.csv` |

### 11.8 双网自动切换实测 + WiFi 踩坑 —— 2026-09-26 ✅ 已实测

**切换实测（三种场景全部通过）**

| 场景 | 实测日志 | 结果 |
|------|----------|------|
| 双链路都在线 | `状态：上行=ETH（IP=192.168.99.26）| 有线可用=1 Wi-Fi可用=1` | ✅ **ETH 优先** |
| **拔出网线** | `上行链路切换：None -> WiFi（IP=192.168.99.32）` | ✅ **WiFi 自动接管** |
| **插回网线** | `上行链路切换：None -> ETH（IP=192.168.99.26）` | ✅ **自动切回 ETH** |

| 项目 | 正确值 | 说明 |
|------|--------|------|
| WiFi 热点 | **`BBC`**（NVS，用户通过 Web 页面配置） | 无需重编译即可换热点 |
| **认证阈值** | **`WIFI_AUTH_WPA_PSK`(2)** | ⚠️ 踩坑：设成 `WPA2_WPA3_PSK`(7) 时，纯 WPA2(3)/WPA-WPA2 混合(4) 的 AP 会被拒 → `reason=211 NO_AP_FOUND_IN_AUTHMODE_THRESHOLD`（日志会提示 `authmode threshold failure (recvd,thresh):(4,7)`） |
| **LVGL 缓冲位置** | **PSRAM**（`flags.buff_spiram = true`） | ⚠️ 踩坑：放在内部 DMA RAM 时，WiFi 连上后会占满 → `Not enough memory for LVGL buffer` → `abort()` **重启循环**；S3 支持 PSRAM DMA，改 PSRAM 后正常 |
| 看板初始化失败处理 | **只告警不 `abort`** | 避免看板一个问题导致整机重启循环、Web 也失联 |
| WiFi 侧 IP | `192.168.99.32`（DHCP） | ETH `192.168.99.26`，Web 控制台两条链路都能访问 |

### 11.9 崩漕排查：内部内存耗尽（LVGL 内置内存池）—— 2026-09-27 ✅ 已修复

| 项目 | 内容 |
|------|------|
| **现象** | 运行约 12 分钟后崩漕重启（看着"无规律"） |
| **诊断手段** | ① `app_main` 每个初始化步后打印 `[heap]` 快照（定位到哪一步吃内存）<br>② `status_task` 每 60s 打印内存（看随时间是否下降）<br>③ `/api/status` 新增 `heap_free` / `heap_min` / `heap_block` / `psram_free`（可远程看趋势） |
| **根因** | LVGL 默认用**内置分配器**（`LV_USE_STDLIB_MALLOC=LV_STDLIB_BUILTIN`）+ **`LV_MEM_SIZE=64KB`**，它从**内部 RAM** 申请 64KB 内存池 → 内部堆只剩 **49KB**、最大连续块 **31KB**、**最低水位一度仅 16 字节** → 运行时稍大一点的分配就失败 → `abort()` 重启 |
| **启动阶段堆画像**（修复前） | netif 223KB → net_eth 213KB → net_wifi 173KB → tf 146KB → mqtt 122KB → **LVGL 66KB（单步吃 56KB）** → web_console **49KB** |
| **修复** | **`CONFIG_LV_USE_CLIB_MALLOC=y`**（改 libc malloc：小块走内部、大块自动进 PSRAM，不再占 64KB 内部池） |
| **效果** | 内部堆 free **49KB → 106KB（+57KB）**；最低水位 **16 字节 → 72KB**；实测连续运行 **>16 分钟**（越过原崩漕点 761s）堆稳定、无崩漕 |
| **教训** | ESP32-S3 上带 WiFi+LVGL 时内部 RAM 很紧：**大缓冲/内存池一定要放 PSRAM**（本项目已两处踩坑：LVGL 绘制缓冲 §11.8、LVGL 内存池 §11.9）；另建议保留内存日志以备日后排查 |

### 11.10 传感器掉线告警 —— 2026-09-27 ✅ 已实测

| 项目 | 值 | 说明 |
|------|-----|------|
| 检测条件 | 超过 `CONFIG_HA_MQTT_SENSOR_ALERT_MS`（默认 **120000 ms**）未收到 `esp32-sensor/data` | 传感器约 15s/条，2 分钟留足容错 |
| 触发动作 | ① 看板弹通知横幅（error 级，红色）② MQTT 发告警 ③ TF 落盘 | 三条链路同时 |
| 告警主题 | **`ha/esp32gw/alert`**（`CONFIG_HA_MQTT_TOPIC_ALERT`） | QoS1，**不 retain**（避免 HA 重启后读到旧告警） |
| 载荷 | `{"type":"sensor_offline\|sensor_recovered","message":"...","uptime":N}` | |
| 去抖 | **仅在状态变化时发一次**（不重复刷屏） | |
| 实测 | 临时改阈值 8s 验证：告警/恢复按预期交替；TF 记录 `2026-09-27 22:44:50,alert,type=sensor_offline`；恢复 2 分钟后 **0 误报** | ✅ |

**HA 侧转发到手机（示例自动化）：**
```yaml
automation:
  - alias: ESP32 传感器掉线告警
    triggers:
      - trigger: mqtt
        topic: ha/esp32gw/alert
    condition:
      - condition: template
        value_template: "{{ trigger.payload_json.type == 'sensor_offline' }}"
    actions:
      - action: notify.mobile_app_你的设备
        data:
          title: "传感器掉线"
          message: "{{ trigger.payload_json.message }}"
```

### 11.11 长时间运行稳定性 —— 2026-09-28 ✅ 用户确认正常

| 项目 | 实测值 | 说明 |
|------|--------|------|
| 连续运行 | **约 20 小时**，MQTT state 实测 uptime `73365s`（约 20.4 小时） | 用户确认期间运行正常，无重启 |
| MQTT | `online=true`，5s 心跳持续 | ETH 上行，设备自报 IP `192.168.99.26` |
| 崩溃修复 | **未再出现 LVGL/内部堆耗尽重启** | 继续使用 libc malloc + PSRAM 缓冲配置 |
| 发现事项 | 长时间运行后 Web TCP 80 曾无响应，但 MQTT 仍正常 | 待恢复串口后定位 `esp_http_server` 长稳问题；已记录在 `BUILD_FLOW.md` |

> 本次长稳观察证明当前内存修复有效；后续修改 Web/网络任务前必须保留 20 小时基线，并重新做长稳验证。

