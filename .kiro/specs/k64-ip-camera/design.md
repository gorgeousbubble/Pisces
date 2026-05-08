# 技术设计文档

## 1. 系统整体架构

```
┌─────────────────────────────────────────────────────────────┐
│                        MCU 端（K64）                         │
│                                                             │
│  ┌──────────┐   DVP/SPI   ┌──────────────┐                 │
│  │Camera    │────────────▶│ 摄像头驱动层  │                 │
│  │Module    │             │ (cam_driver)  │                 │
│  └──────────┘             └──────┬───────┘                 │
│                                  │ 原始帧 (YUV/RGB)         │
│                           ┌──────▼───────┐                 │
│                           │ JPEG 编码器   │                 │
│                           │(jpeg_encoder) │                 │
│                           └──────┬───────┘                 │
│                    ┌─────────────┼─────────────┐           │
│                    │ JPEG帧      │             │ JPEG帧     │
│             ┌──────▼──────┐     │      ┌──────▼──────┐    │
│             │ 文件管理器   │     │      │  网络传输层  │    │
│             │(file_mgr)   │     │      │(net_stack)  │    │
│             └──────┬──────┘     │      └──────┬──────┘    │
│                    │            │             │            │
│             ┌──────▼──────┐     │      ┌──────▼──────┐    │
│             │  SD卡        │     │      │  WiFi模块   │    │
│             │ (SDHC/FAT32)│     │      │(ESP8266/32) │    │
│             └─────────────┘     │      └──────┬──────┘    │
│                                 │             │            │
│                          ┌──────▼──────┐      │            │
│                          │  系统管理器  │      │            │
│                          │(sys_manager)│      │            │
│                          └─────────────┘      │            │
└────────────────────────────────────────────────┼────────────┘
                                                 │ TCP/HTTP (内网)
                                    ┌────────────▼────────────┐
                                    │    树莓派 5 服务器端      │
                                    │                         │
                                    │  ┌───────────────────┐  │
                                    │  │  视频流接收器       │  │
                                    │  │ (stream_receiver)  │  │
                                    │  └────────┬──────────┘  │
                                    │           │              │
                                    │  ┌────────▼──────────┐  │
                                    │  │  HTTP API 服务器   │  │
                                    │  │  (FastAPI/Flask)   │  │
                                    │  └────────┬──────────┘  │
                                    │           │              │
                                    │  ┌────────▼──────────┐  │
                                    │  │  本地文件存储       │  │
                                    │  │  /var/ipcam/       │  │
                                    │  └───────────────────┘  │
                                    └─────────────┬───────────┘
                                                  │ HTTP (内网/外网)
                                    ┌─────────────▼───────────┐
                                    │      Web 客户端           │
                                    │  (浏览器 / 移动端 App)    │
                                    └─────────────────────────┘
```

---

## 2. MCU 固件架构

### 2.1 软件层次结构

```
┌─────────────────────────────────────────────────────┐
│                   应用层 (Application)               │
│  main_task | cmd_handler | status_reporter           │
├─────────────────────────────────────────────────────┤
│                   服务层 (Service)                   │
│  jpeg_encoder | file_mgr | net_stack | sys_manager  │
├─────────────────────────────────────────────────────┤
│                   驱动层 (Driver)                    │
│  cam_driver | sdhc_driver | uart_wifi_driver         │
├─────────────────────────────────────────────────────┤
│              硬件抽象层 (HAL / SDK)                  │
│  NXP KSDK 2.x  |  FreeRTOS  |  FatFs               │
└─────────────────────────────────────────────────────┘
```

### 2.2 RTOS 任务划分

| 任务名称 | 优先级 | 栈大小 | 职责 |
|---------|--------|--------|------|
| `task_cam_capture` | 高(4) | 2KB | 驱动摄像头采集，将原始帧写入帧缓冲队列 |
| `task_jpeg_encode` | 高(4) | 4KB | 从帧缓冲队列取帧，JPEG编码，写入编码队列 |
| `task_net_send` | 中(3) | 3KB | 从编码队列取帧，通过WiFi推送至服务器 |
| `task_file_write` | 中(3) | 3KB | 从编码队列取帧，写入SD卡 |
| `task_cmd_handler` | 中(3) | 2KB | 处理来自服务器的控制命令（拍照、配置等）|
| `task_sys_manager` | 低(2) | 2KB | 看门狗喂狗、状态上报、错误恢复 |
| `task_watchdog` | 最高(5) | 1KB | 独立看门狗喂狗任务，监控主循环健康 |

### 2.3 内存布局（256KB RAM）

```
地址空间分配（示意）：
┌──────────────────────────────────┐ 0x2000_0000
│  .data / .bss (全局变量)  ~16KB  │
├──────────────────────────────────┤
│  FreeRTOS 内核 + 任务栈  ~32KB   │
├──────────────────────────────────┤
│  帧缓冲区 (raw_frame_buf)        │
│  2 × 640×480×2(YUV422) = 1.2MB  │ ← 超出RAM，需降分辨率或分块处理
│  实际采用分块传输：               │
│  2 × 640×16×2 = 40KB 行缓冲     │
├──────────────────────────────────┤
│  JPEG 输出缓冲区  ~40KB          │
│  (单帧VGA JPEG平均30KB)          │
├──────────────────────────────────┤
│  FatFs 工作缓冲区  ~8KB          │
├──────────────────────────────────┤
│  WiFi AT指令缓冲区  ~4KB         │
├──────────────────────────────────┤
│  系统日志环形缓冲区  ~4KB         │
├──────────────────────────────────┤
│  预留/堆  ~剩余                  │
└──────────────────────────────────┘ 0x2003_FFFF
```

> **关键约束**：VGA原始帧(640×480×2=614KB)远超RAM，必须采用**行扫描分块编码**策略：摄像头通过DMA逐行输出，JPEG编码器以16行为单位处理，避免整帧缓存。

### 2.4 帧数据流与队列设计

```
cam_driver
    │ DMA行中断，16行一块
    ▼
[raw_line_buf: 双缓冲，各 640×16×2 = 20KB]
    │ 行块就绪信号
    ▼
task_jpeg_encode（行扫描JPEG编码）
    │ 编码完成，输出完整JPEG帧
    ▼
[encoded_frame_queue: 深度=2，每帧最大40KB]
    ├──────────────────────────┐
    ▼                          ▼
task_net_send              task_file_write
（HTTP推流）               （SD卡写入）
```

---

## 3. 硬件接口设计

### 3.1 引脚分配（MK64FN1M0VLL12 LQFP-100）

| 功能 | 接口 | K64 引脚 | 说明 |
|------|------|---------|------|
| 摄像头数据 D0–D7 | GPIO（DVP） | PTD0–PTD7 | 8位并行数据 |
| 摄像头 PCLK | GPIO输入 | PTC3 | 像素时钟 |
| 摄像头 VSYNC | GPIO输入 | PTC2 | 帧同步 |
| 摄像头 HREF | GPIO输入 | PTC1 | 行同步 |
| 摄像头 XCLK | FTM输出 | PTC4 | 主时钟（24MHz） |
| 摄像头 SCCB SDA | I2C0 SDA | PTB1 | 摄像头配置 |
| 摄像头 SCCB SCL | I2C0 SCL | PTB0 | 摄像头配置 |
| WiFi UART TX | UART1 TX | PTC4 | MCU→WiFi |
| WiFi UART RX | UART1 RX | PTC3 | WiFi→MCU |
| WiFi RST | GPIO输出 | PTA13 | WiFi硬件复位 |
| WiFi EN | GPIO输出 | PTA12 | WiFi使能 |
| SD卡 CLK | SDHC CLK | PTE2 | SDHC时钟 |
| SD卡 CMD | SDHC CMD | PTE3 | SDHC命令 |
| SD卡 D0–D3 | SDHC D0–D3 | PTE1,PTE0,PTD13,PTD12 | 4位数据 |
| SD卡 CD | GPIO输入 | PTE6 | 卡检测 |
| 状态LED | GPIO输出 | PTB22 | 系统状态指示 |
| 错误LED | GPIO输出 | PTE26 | 错误状态指示 |

### 3.2 摄像头模块选型

推荐使用 **OV2640**（200万像素，支持DVP，内置JPEG硬件编码器）：
- 支持输出格式：JPEG、YUV422、RGB565
- 最大分辨率：1600×1200（UXGA），VGA模式下可达30fps
- 内置JPEG编码器可大幅降低MCU软件编码压力
- SCCB（兼容I2C）配置接口

> **设计决策**：优先使用OV2640的**片上JPEG编码**输出，MCU直接接收JPEG数据，绕过软件编码瓶颈，解决256KB RAM不足以缓存原始帧的问题。

### 3.3 WiFi 模块选型

推荐使用 **ESP8266-01S** 或 **ESP-WROOM-02**：
- AT指令集通信（UART，波特率115200）
- 支持TCP客户端模式
- 支持透传模式（CIPMODE=1）用于视频流推送

---

## 4. 关键模块详细设计

### 4.1 摄像头驱动（cam_driver）

```c
/* 接口定义 */
typedef struct {
    uint8_t  *data;      /* JPEG数据指针（OV2640直接输出JPEG） */
    uint32_t  size;      /* JPEG数据字节数 */
    uint32_t  frame_id;  /* 帧序号 */
    uint32_t  timestamp; /* 采集时间戳（ms） */
} cam_frame_t;

cam_status_t cam_init(const cam_config_t *cfg);
cam_status_t cam_start_capture(void);
cam_status_t cam_stop_capture(void);
cam_status_t cam_get_frame(cam_frame_t *frame, uint32_t timeout_ms);
cam_status_t cam_reinit(void);
uint32_t     cam_get_drop_count(void);
```

**采集流程**：
1. I2C配置OV2640寄存器（分辨率、JPEG质量、帧率）
2. 启动DVP+DMA，OV2640输出JPEG数据流
3. VSYNC中断标记帧开始/结束
4. DMA完成中断将帧数据指针放入 `frame_queue`
5. 超时（66ms）未收到帧完成信号则丢帧，Drop_Counter++

### 4.2 JPEG 编码器（jpeg_encoder）

由于使用OV2640片上JPEG，此模块主要负责：
- 质量参数配置（通过SCCB写入OV2640 QS寄存器）
- 拍照时切换高分辨率（1280×720）并重新配置
- 维护 Drop_Counter

```c
typedef struct {
    uint8_t  quality;    /* JPEG质量因子 50–95，映射到OV2640 QS寄存器 */
    uint16_t width;      /* 输出宽度 */
    uint16_t height;     /* 输出高度 */
} encoder_config_t;

enc_status_t encoder_init(const encoder_config_t *cfg);
enc_status_t encoder_set_quality(uint8_t quality);
enc_status_t encoder_set_resolution(uint16_t w, uint16_t h);
uint32_t     encoder_get_drop_count(void);
```

### 4.3 文件管理器（file_mgr）

基于 **FatFs** 库实现，运行于 `task_file_write`。

```c
/* 文件命名规则 */
/* 录像：REC_YYYYMMDD_HHMMSS.mjpeg */
/* 照片：SNAP_YYYYMMDD_HHMMSS.jpg  */

fm_status_t  fm_init(void);
fm_status_t  fm_start_recording(void);   /* 创建新录像文件 */
fm_status_t  fm_stop_recording(void);    /* 关闭当前录像文件 */
fm_status_t  fm_write_frame(const uint8_t *data, uint32_t size);
fm_status_t  fm_save_snapshot(const uint8_t *data, uint32_t size, char *out_path);
fm_status_t  fm_get_free_space(uint64_t *free_bytes);
fm_status_t  fm_list_recordings(fm_file_info_t *list, uint32_t *count);
```

**MJPEG 文件格式**：每帧前写4字节长度头 + JPEG数据，便于服务器端解析：
```
[4B: frame_size][JPEG data][4B: frame_size][JPEG data]...
```

**文件轮转**：当文件大小 ≥ 3.9GB 时自动关闭并新建文件。

### 4.4 网络传输层（net_stack）

通过 UART AT 指令驱动 ESP8266/ESP32。

```c
/* 连接状态机 */
typedef enum {
    NET_STATE_IDLE,
    NET_STATE_WIFI_CONNECTING,
    NET_STATE_WIFI_CONNECTED,
    NET_STATE_TCP_CONNECTING,
    NET_STATE_STREAMING,
    NET_STATE_OFFLINE,
    NET_STATE_ERROR
} net_state_t;

net_status_t net_init(const net_config_t *cfg);
net_status_t net_connect(void);           /* WiFi + TCP 连接 */
net_status_t net_send_frame(const uint8_t *jpeg, uint32_t size);
net_status_t net_send_snapshot(const uint8_t *jpeg, uint32_t size);
net_status_t net_send_status(const sys_status_t *status);
net_status_t net_reset_wifi_module(void); /* 拉低RST引脚100ms */
net_state_t  net_get_state(void);
```

**视频流推送协议**（MJPEG over HTTP）：
```
POST /stream HTTP/1.1\r\n
Host: <server_ip>:<port>\r\n
Content-Type: multipart/x-mixed-replace; boundary=frame\r\n
\r\n
--frame\r\n
Content-Type: image/jpeg\r\n
Content-Length: <size>\r\n
\r\n
<JPEG data>
--frame\r\n
...
```

**命令接收**：MCU 同时维护一个 TCP 监听连接（或复用推流连接的反向通道），接收服务器下发的 JSON 命令：
```json
{"cmd": "snapshot", "quality": 85, "width": 1280, "height": 720}
{"cmd": "set_fps", "fps": 15}
{"cmd": "set_quality", "quality": 75}
```

### 4.5 系统管理器（sys_manager）

```c
typedef struct {
    net_state_t  net_state;
    uint8_t      fps_current;
    uint64_t     sd_free_mb;
    uint32_t     uptime_sec;
    uint32_t     drop_count;
    bool         cam_available;
    bool         sd_available;
} sys_status_t;

void sys_manager_task(void *param);  /* FreeRTOS任务入口 */
void sys_watchdog_feed(void);        /* 喂狗 */
void sys_report_status(void);        /* 上报状态至服务器 */
void sys_handle_cam_error(void);     /* 摄像头错误恢复 */
void sys_handle_wifi_error(void);    /* WiFi错误恢复 */
```

**看门狗策略**：使用 K64 内置 WDOG 模块，超时 5 秒。`task_sys_manager` 每 1 秒喂狗，同时检查各任务心跳标志位，若任意任务超过 3 秒未更新心跳则主动触发软复位。

---

## 5. 树莓派服务器端架构

### 5.1 技术栈

| 组件 | 技术选型 | 说明 |
|------|---------|------|
| Web 框架 | **FastAPI** (Python 3.11+) | 异步HTTP，性能好，自动生成API文档 |
| ASGI 服务器 | **Uvicorn** | 生产级异步服务器 |
| 视频流接收 | 自定义 TCP 服务（asyncio） | 接收MCU推送的MJPEG流 |
| 反向代理 | **Nginx** | 外网访问、SSL终止、Basic Auth |
| 存储 | 本地文件系统 `/var/ipcam/` | 录像和照片持久化 |
| 数据库 | **SQLite** | 录像文件索引、状态历史 |
| 进程管理 | **systemd** | 服务自启动和守护 |

### 5.2 服务器目录结构

```
/var/ipcam/
├── recordings/          # 录像文件（从MCU SD卡同步或直接接收）
│   ├── REC_20260508_120000.mjpeg
│   └── ...
├── snapshots/           # 照片文件
│   ├── SNAP_20260508_120500.jpg
│   └── ...
└── ipcam.db             # SQLite 索引数据库

/opt/ipcam-server/
├── main.py              # FastAPI 应用入口
├── stream_receiver.py   # TCP流接收器
├── api/
│   ├── stream.py        # 实时视频流接口
│   ├── recordings.py    # 录像管理接口
│   ├── snapshots.py     # 拍照接口
│   └── status.py        # 状态查询接口
├── models.py            # 数据模型
├── config.py            # 配置管理
└── requirements.txt
```

### 5.3 REST API 设计

#### 实时视频流
```
GET  /stream/live
     → Content-Type: multipart/x-mixed-replace; boundary=frame
     → 需要 Basic Auth（外网模式）
     → 503 若MCU未连接
```

#### 录像管理
```
GET  /api/recordings?start=<ISO8601>&end=<ISO8601>
     → 200 {"recordings": [{"name": "...", "timestamp": "...", "size": 12345}]}
     → 400 参数格式错误

GET  /api/recordings/{filename}
     → 200 Transfer-Encoding: chunked（流式下载/播放）
     → 404 文件不存在
```

#### 拍照
```
POST /api/snapshot
     Body: {"quality": 85}（可选）
     → 200 {"url": "/api/snapshots/SNAP_20260508_120500.jpg"}
     → 504 MCU超时未响应

GET  /api/snapshots/{filename}
     → 200 image/jpeg
     → 404 文件不存在
```

#### 状态查询
```
GET  /api/status
     → 200 {
         "mcu_online": true,
         "wifi_state": "connected",
         "fps": 15,
         "sd_free_mb": 12345,
         "uptime_sec": 3600,
         "drop_count": 0,
         "last_seen": "2026-05-08T12:05:00Z"
       }
```

### 5.4 视频流接收与转发架构

```
MCU (TCP Client)
    │ MJPEG over HTTP POST /stream
    ▼
stream_receiver.py (asyncio TCP Server, port 8554)
    │ 解析MJPEG帧边界
    ▼
frame_buffer (asyncio.Queue, maxsize=5)
    │
    ├──▶ 广播给所有已连接的 Web_Client（最多10个）
    │    GET /stream/live
    │
    └──▶ task_file_writer（可选：服务器端录像备份）
```

### 5.5 外网访问方案

```
外网用户
    │ HTTPS:443
    ▼
Nginx（反向代理 + SSL + Basic Auth）
    │ HTTP:8000（内部）
    ▼
FastAPI / Uvicorn
```

路由器端口映射：外网 TCP 443 → 树莓派 IP:443

SSL 证书：使用 Let's Encrypt（需要域名）或自签名证书。

---

## 6. 数据库模型（SQLite）

```sql
-- 录像文件索引
CREATE TABLE recordings (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    filename    TEXT NOT NULL UNIQUE,
    start_time  TEXT NOT NULL,  -- ISO 8601
    end_time    TEXT,
    size_bytes  INTEGER,
    storage     TEXT DEFAULT 'sd'  -- 'sd' | 'server'
);

-- 照片索引
CREATE TABLE snapshots (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    filename    TEXT NOT NULL UNIQUE,
    taken_at    TEXT NOT NULL,  -- ISO 8601
    size_bytes  INTEGER,
    sd_saved    INTEGER DEFAULT 1  -- 0=SD写入失败
);

-- MCU状态历史（最近100条）
CREATE TABLE mcu_status_log (
    id          INTEGER PRIMARY KEY AUTOINCREMENT,
    recorded_at TEXT NOT NULL,
    fps         INTEGER,
    sd_free_mb  INTEGER,
    drop_count  INTEGER,
    net_state   TEXT
);
```

---

## 7. 配置文件设计

### 7.1 MCU 端（SD卡 config.ini）

```ini
[wifi]
ssid     = MyHomeWiFi
password = mypassword123

[server]
ip       = 192.168.1.100
port     = 8554

[camera]
fps      = 15
quality  = 75
```

### 7.2 服务器端（config.py）

```python
MCU_STREAM_PORT   = 8554    # 接收MCU推流的TCP端口
API_PORT          = 8000    # FastAPI HTTP端口
MAX_CLIENTS       = 10      # 最大并发Web客户端数
STORAGE_PATH      = "/var/ipcam"
SD_LOW_SPACE_MB   = 50
AUTH_ENABLED      = True    # 外网访问认证开关
AUTH_USERNAME     = "admin"
AUTH_PASSWORD     = "changeme"  # 生产环境使用环境变量
```

---

## 8. 错误处理与恢复机制

| 故障场景 | 检测方式 | 恢复动作 |
|---------|---------|---------|
| 摄像头初始化失败 | cam_init() 返回错误 | 重试3次，失败后上报并等待WDG复位 |
| 连续10帧采集超时 | 帧超时计数器 | 重新初始化摄像头，记录日志 |
| JPEG编码超时 | 编码计时器 | 丢帧，Drop_Counter++，继续下帧 |
| SD卡未插入 | FatFs f_mount() 失败 | 禁用本地存储，仅网络传输 |
| SD卡空间不足 | f_getfree() < 50MB | 停止录像，发送告警 |
| WiFi连接失败 | AT+CWJAP 超时 | 重试5次（间隔10s），进入离线模式 |
| TCP连接断开 | AT+CIPSTATUS 检测 | 重试5次（间隔5s），进入离线模式 |
| WiFi模块无响应60s | 心跳检测 | 拉低RST引脚100ms，重新连接 |
| 主循环阻塞 | 看门狗5s超时 | 硬件复位，重新初始化所有外设 |

---

## 9. 正确性属性（用于测试验证）

1. **帧单调性**：`frame_id` 严格递增，不允许重复或乱序。
2. **Drop_Counter 一致性**：Drop_Counter 只增不减，等于所有丢帧事件之和。
3. **文件命名唯一性**：同一秒内不会产生两个同名文件（时间戳精度到秒，需加序号后缀保护）。
4. **队列无死锁**：`encoded_frame_queue` 的生产者（编码任务）和消费者（网络/文件任务）不会互相等待。
5. **看门狗活性**：系统正常运行时，喂狗间隔始终 < 5s。
6. **API 幂等性**：`POST /api/snapshot` 每次调用产生独立文件，不覆盖历史照片。
7. **并发安全**：多个 Web_Client 同时访问 `/stream/live` 时，frame_buffer 的读取不会产生数据竞争。
