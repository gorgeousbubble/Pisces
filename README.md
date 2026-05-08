# Pisces — 家用网络摄像头监控系统

基于 NXP **MK64FN1M0VLL12** 微控制器的嵌入式网络摄像头监控方案，配合家庭部署的**树莓派 5** 服务器，实现实时视频查看、历史录像回放和远程拍照。

---

## 系统概览

```
┌─────────────────────┐        WiFi / TCP        ┌──────────────────────┐
│   MCU 端（K64）      │ ─────────────────────▶  │  树莓派 5 服务器      │
│                     │                          │                      │
│  OV2640 摄像头       │   MJPEG-over-HTTP 推流   │  FastAPI + Nginx      │
│  SDHC SD 卡存储      │ ◀─────────────────────  │  SQLite 索引          │
│  ESP8266 WiFi 模块   │   JSON 控制命令           │  /var/ipcam/ 存储     │
└─────────────────────┘                          └──────────┬───────────┘
                                                            │ HTTP/HTTPS
                                                 ┌──────────▼───────────┐
                                                 │  Web 客户端           │
                                                 │  浏览器 / 移动端 App  │
                                                 └──────────────────────┘
```

### 主要功能

- **实时视频查看**：MJPEG 流，支持 3–10 路并发客户端，帧率 ≥ 10fps
- **本地录像存储**：FAT32 SD 卡，MJPEG 格式，自动文件轮转（3.9GB/文件）
- **历史录像回放**：按时间范围筛选，HTTP 流式传输，支持在线播放
- **远程拍照**：1280×720 高分辨率，端到端 3 秒内响应
- **内外网访问**：内网直连 + Nginx 反向代理 + HTTPS + Basic Auth 外网访问
- **自动错误恢复**：看门狗、摄像头重初始化、WiFi 硬件复位

---

## 硬件规格

| 组件 | 型号 / 规格 |
|------|------------|
| 主控 MCU | NXP MK64FN1M0VLL12（ARM Cortex-M4，120MHz，1MB Flash，256KB RAM） |
| 摄像头 | OV2640（200万像素，DVP 接口，片上 JPEG 编码，VGA@15fps） |
| WiFi 模块 | ESP8266-01S / ESP-WROOM-02（AT 指令，UART 115200） |
| 存储 | MicroSD 卡（FAT32，SDHC，4 位总线，25MHz） |
| 服务器 | 树莓派 5（4GB+，运行 Raspberry Pi OS） |
| 调试接口 | SWD（SWDIO=PTA3，SWDCLK=PTA0），串口日志 UART0 115200 |

---

## 目录结构

```
pisces/
├── docs/                        # 项目文档
│   ├── README.md                # 文档索引
│   ├── requirements.md          # 需求文档（9 个需求，45 条验收标准）
│   ├── design.md                # 技术设计文档（架构、模块、API）
│   └── tasks.md                 # 实现任务列表（17 个任务，~21 工作日）
│
├── firmware/                    # MCU 固件（C 语言，NXP KSDK + FreeRTOS）
│   ├── include/                 # 公共头文件
│   │   ├── board.h              # 板级引脚定义
│   │   ├── ipcam_config.h       # 运行时配置结构体与默认值
│   │   ├── ipcam_types.h        # 公共类型定义
│   │   ├── FreeRTOSConfig.h     # FreeRTOS 配置
│   │   ├── cam_driver.h         # 摄像头驱动接口
│   │   ├── file_mgr.h           # 文件管理器接口
│   │   ├── net_stack.h          # 网络传输层接口
│   │   ├── sys_manager.h        # 系统管理器接口
│   │   ├── config_loader.h      # 配置加载器接口
│   │   └── log.h                # 日志模块接口
│   │
│   ├── src/
│   │   ├── app/
│   │   │   └── main.c           # 系统入口，FreeRTOS 任务创建
│   │   ├── driver/
│   │   │   └── cam_driver.c     # OV2640 驱动（SCCB + DVP + DMA）
│   │   ├── hal/
│   │   │   └── board.c          # 板级初始化（时钟 120MHz，引脚复用）
│   │   └── service/
│   │       ├── log.c            # 串口日志（带时间戳，Mutex 保护）
│   │       ├── config_loader.c  # INI 配置文件解析
│   │       ├── sys_manager.c    # 看门狗、心跳监控、Drop_Counter
│   │       ├── file_mgr.c       # FatFs 文件管理（录像/照片/轮转）
│   │       └── net_stack.c      # ESP8266 AT 指令驱动，MJPEG 推流
│   │
│   ├── ide/
│   │   ├── keil/
│   │   │   └── k64_ipcam.uvprojx   # Keil MDK 5 工程文件
│   │   └── iar/
│   │       ├── k64_ipcam.eww        # IAR EWARM 工作区
│   │       ├── k64_ipcam.ewp        # IAR EWARM 工程文件
│   │       └── MK64FN1M0xxx12_flash.icf  # IAR 链接脚本
│   │
│   ├── third_party/             # 第三方库（需手动下载，见 README）
│   │   ├── FreeRTOS/            # FreeRTOS 10.5.1
│   │   ├── FatFs/               # FatFs R0.15（含 diskio.c SDHC 适配）
│   │   └── KSDK/                # NXP MCUXpresso SDK 2.x
│   │
│   └── README.md                # 固件工程说明与第三方库安装指南
│
└── server/                      # 树莓派服务器端（Python，待实现）
    └── ...
```

---

## 快速开始

### 固件构建

**前置条件：**
- Keil MDK ≥ 5.38（ARM Compiler 6）或 IAR EWARM ≥ 9.30
- NXP MCUXpresso SDK 2.x for MK64F12
- FreeRTOS 10.5.1、FatFs R0.15（见 `firmware/third_party/README.md`）

**步骤：**

1. 按照 `firmware/third_party/README.md` 下载并放置第三方库
2. 在 SD 卡根目录创建 `config.ini`（参考下方配置示例）
3. 用 Keil5 打开 `firmware/ide/keil/k64_ipcam.uvprojx`，或用 IAR 打开 `firmware/ide/iar/k64_ipcam.eww`
4. 编译并通过 SWD 烧录到目标板

**SD 卡配置文件 `config.ini`：**

```ini
[wifi]
ssid     = 你的WiFi名称
password = 你的WiFi密码

[server]
ip       = 192.168.1.100
port     = 8554

[camera]
fps      = 15
quality  = 75
```

### 串口日志

连接 UART0（PTA14=TX，PTA15=RX，115200 8N1），上电后可看到启动日志：

```
========================================
  K64 IP Camera Firmware v1.0.0
  NXP MK64FN1M0VLL12 @ 120MHz
========================================
[       0][I][MAIN    ] === K64 IP Camera Firmware Starting ===
[       5][I][FM      ] SD card mounted OK, free space: 28672 MB
[      12][I][CFG     ] config.ini loaded OK: ssid=MyHomeWiFi server=192.168.1.100:8554 fps=15 quality=75
[      45][I][CAM     ] OV2640 initialized: res=VGA quality=75 fps=15
[      80][I][NET     ] Network layer initialized
[     120][I][SYS     ] Watchdog initialized, timeout=5000ms
[     125][I][MAIN    ] All tasks created, starting scheduler
[     580][I][NET     ] WiFi connected
[     620][I][NET     ] TCP connected, streaming started
```

### 服务器端部署

> 服务器端代码正在实现中，详见 `docs/tasks.md` 阶段四。

---

## 引脚分配速查

| 功能 | K64 引脚 | 说明 |
|------|---------|------|
| 摄像头数据 D0–D7 | PTD0–PTD7 | DVP 8 位并行 |
| 摄像头 PCLK / VSYNC / HREF | PTC3 / PTC2 / PTC1 | 帧同步信号 |
| 摄像头 XCLK | PTC4（FTM0_CH3） | 24MHz 主时钟 |
| 摄像头 SCCB | PTB0（SCL）/ PTB1（SDA） | I2C0 400kHz |
| WiFi UART | PTC4（TX）/ PTC3（RX） | UART1 115200 |
| WiFi RST / EN | PTA13 / PTA12 | 控制引脚 |
| SD 卡 SDHC | PTE0–PTE3，PTD12–PTD13 | 4 位总线 |
| SD 卡检测 | PTE6 | 低电平 = 已插入 |
| 状态 LED（绿） | PTB22 | 低有效 |
| 错误 LED（红） | PTE26 | 低有效 |
| SWD 调试 | PTA0（CLK）/ PTA3（DIO） | J-Link / CMSIS-DAP |
| 串口日志 | PTA14（TX）/ PTA15（RX） | UART0 115200 |

---

## 文档

- [需求文档](docs/requirements.md)
- [技术设计文档](docs/design.md)
- [实现任务列表](docs/tasks.md)
- [固件工程说明](firmware/README.md)

---

## 开发状态

| 模块 | 状态 |
|------|------|
| MCU 固件 — 板级初始化 | ✅ 完成 |
| MCU 固件 — 日志模块 | ✅ 完成 |
| MCU 固件 — 配置加载 | ✅ 完成 |
| MCU 固件 — 系统管理器 / 看门狗 | ✅ 完成 |
| MCU 固件 — OV2640 摄像头驱动 | ✅ 完成 |
| MCU 固件 — SD 卡 / 文件管理器 | ✅ 完成 |
| MCU 固件 — WiFi 网络层 | ✅ 完成 |
| MCU 固件 — 主任务调度 | ✅ 完成 |
| MCU 固件 — FatFs SDHC 底层驱动 | ✅ 完成 |
| Keil5 工程文件 | ✅ 完成 |
| IAR EWARM 工程文件 | ✅ 完成 |
| 树莓派服务器端 | 🔲 待实现 |
| 端到端集成测试 | 🔲 待实现 |

---

## License

[MIT](LICENSE)
