# 需求文档

## 简介

本系统是一个基于 NXP MK64FN1M0VLL12 微控制器的家用网络摄像头监控系统。系统通过摄像头模块采集实时影像，将数据存储至本地 SD 卡，并通过 WiFi 模块将视频/图像数据传输至家庭内网中的树莓派 5 服务器。用户可通过内网或外网访问树莓派服务器，实现实时视频查看、历史录像回放及远程拍照功能。

## 词汇表

- **MCU**：微控制器单元，本系统中指 NXP MK64FN1M0VLL12（ARM Cortex-M4，120MHz，1MB Flash，256KB RAM）
- **Camera_Module**：摄像头模块，负责采集图像和视频帧，通过 DVP（数字视频并行接口）或 SPI 与 MCU 连接
- **WiFi_Module**：WiFi 无线通信模块（如 ESP8266/ESP32），通过 UART/SPI 与 MCU 通信，负责将数据传输至家庭内网
- **SD_Controller**：MCU 内置的 SDHC 控制器，负责管理 SD 卡的读写操作
- **SD_Card**：本地存储介质，用于存储录像文件和照片
- **Raspberry_Pi_Server**：家庭部署的树莓派 5 服务器，作为后端管理平台，负责接收、存储、转发视频流和图像数据
- **Stream_Encoder**：MCU 上的软件模块，负责将原始图像帧压缩编码为 MJPEG 格式
- **File_Manager**：MCU 上的文件系统管理模块，负责 SD 卡上文件的创建、写入和索引
- **Network_Stack**：MCU 上的网络协议栈模块，负责 TCP/IP 通信和 HTTP 协议处理
- **Web_Client**：用户通过浏览器或移动端 App 访问系统的客户端
- **Frame**：单帧图像数据，分辨率不低于 640×480（VGA）
- **Recording**：连续帧序列组成的录像文件，以 MJPEG 格式存储
- **Snapshot**：单张静态照片，以 JPEG 格式存储
- **Drop_Counter**：MCU 上维护的可读计数器，记录因内存不足或超时而丢弃的帧总数

---

## 需求

### 需求 1：摄像头图像采集

**用户故事：** 作为系统管理员，我希望 MCU 能持续采集摄像头图像帧，以便为实时预览和录像功能提供原始数据。

#### 验收标准

1. THE Camera_Module SHALL 以不低于 15fps 的帧率持续输出图像帧。
2. THE Camera_Module SHALL 输出分辨率不低于 640×480（VGA）的图像帧。
3. WHEN Camera_Module 完成一帧图像采集，THE MCU SHALL 在 50ms 内将该帧读入内部缓冲区；若超过 50ms 未完成读取，THE MCU SHALL 丢弃该帧并将 Drop_Counter 加 1。
4. IF Camera_Module 初始化失败，THEN THE MCU SHALL 记录包含失败原因和时间戳的错误日志，停止后续采集流程，并通过 WiFi_Module 向 Raspberry_Pi_Server 上报包含错误码的状态消息。
5. WHILE MCU 处于低功耗待机模式，THE Camera_Module SHALL 停止图像采集以降低功耗。
6. IF Camera_Module 在重新初始化后连续 3 次仍初始化失败，THEN THE MCU SHALL 停止重试并保持错误状态，等待看门狗复位或人工干预。

---

### 需求 2：图像编码与压缩

**用户故事：** 作为系统管理员，我希望 MCU 能将原始图像帧压缩为 MJPEG 格式，以便在有限带宽和存储空间下传输和保存视频数据。

#### 验收标准

1. WHEN MCU 接收到一帧原始图像且质量因子配置参数已加载，THE Stream_Encoder SHALL 将其压缩为 JPEG 格式，压缩质量因子取自配置参数（范围 50–95，默认值 75）。
2. WHEN MCU 接收到一帧 640×480 原始图像，THE Stream_Encoder SHALL 在 200ms 内完成 JPEG 编码；IF 编码耗时超过 200ms，THEN THE Stream_Encoder SHALL 丢弃该帧，将 Drop_Counter 加 1，并记录包含帧序号和实际耗时的超时日志。
3. IF 编码过程中发生内存不足错误，THEN THE Stream_Encoder SHALL 丢弃当前帧，将 Drop_Counter 加 1，并在 66ms 内（即下一帧周期内）恢复对后续帧的处理。
4. THE Stream_Encoder SHALL 输出符合 JFIF 标准的 JPEG 数据，以确保与标准图像查看器的兼容性。
5. IF 编码输出缓冲区已满，THEN THE Stream_Encoder SHALL 丢弃当前帧，将 Drop_Counter 加 1，并记录缓冲区溢出事件，不阻塞后续帧的编码流程。

---

### 需求 3：本地存储管理

**用户故事：** 作为用户，我希望系统能将录像和照片存储到 SD 卡，以便在网络中断时仍可保留历史数据。

#### 验收标准

1. THE File_Manager SHALL 在 SD 卡上以 FAT32 文件系统格式组织存储文件。
2. WHEN 系统进入录像模式，THE File_Manager SHALL 在 SD 卡上创建以当前时间戳命名的录像文件（格式：`REC_YYYYMMDD_HHMMSS.mjpeg`）。
3. WHEN 用户触发拍照指令，THE File_Manager SHALL 在 SD 卡上创建以当前时间戳命名的照片文件（格式：`SNAP_YYYYMMDD_HHMMSS.jpg`）。
4. WHILE 系统处于录像模式，THE SD_Controller SHALL 以不低于 4MB/s 的持续写入速率将编码后的帧数据写入 SD 卡。
5. IF SD 卡剩余空间低于 50MB，THEN THE File_Manager SHALL 完成当前正在写入的录像文件（写入文件结束标记并关闭），停止创建新录像文件，并通过 WiFi_Module 向 Raspberry_Pi_Server 发送存储空间不足告警。
6. IF SD 卡写入操作失败，THEN THE File_Manager SHALL 记录包含失败原因和帧序号的错误日志，跳过当前帧写入，不中断视频流传输。
7. IF 系统启动时检测到 SD 卡未插入或无法挂载，THEN THE File_Manager SHALL 记录错误日志，禁用本地存储功能，并通过 WiFi_Module 向 Raspberry_Pi_Server 上报 SD 卡不可用状态。
8. IF 录像模式下持续写入速率低于 4MB/s 超过 5 秒，THEN THE File_Manager SHALL 记录写入性能告警日志，并通过 WiFi_Module 向 Raspberry_Pi_Server 上报写入性能降级状态。
9. WHEN 当前录像文件大小达到 3.9GB（FAT32 单文件上限 4GB 的安全阈值），THE File_Manager SHALL 关闭当前录像文件并自动创建新的录像文件（以新时间戳命名），保证录像连续性。

---

### 需求 4：WiFi 无线传输

**用户故事：** 作为用户，我希望 MCU 能通过 WiFi 将实时视频流传输至树莓派服务器，以便在家庭内网中访问摄像头画面。

#### 验收标准

1. WHEN 系统上电启动，THE Network_Stack SHALL 通过 WiFi_Module 连接至预配置的家庭 WiFi 接入点，连接超时时间为 30 秒。
2. IF WiFi 连接失败，THEN THE Network_Stack SHALL 每隔 10 秒重试连接，最多重试 5 次；超过重试次数后，THE Network_Stack SHALL 记录错误日志并进入离线存储模式。
3. WHEN WiFi 连接建立成功，THE Network_Stack SHALL 向 Raspberry_Pi_Server 发起 TCP 连接，目标端口取自配置参数。
4. WHILE WiFi 连接处于活跃状态，THE Network_Stack SHALL 以 MJPEG-over-HTTP 格式持续向 Raspberry_Pi_Server 推送编码后的视频帧，推送帧率不低于 15fps。
5. THE Network_Stack SHALL 保证每一帧 JPEG 数据从编码完成到送达 Raspberry_Pi_Server 的传输延迟不超过 500ms（基于家庭内网 2.4GHz WiFi 环境）。
6. IF TCP 连接意外断开，THEN THE Network_Stack SHALL 每隔 5 秒尝试重新建立连接，最多重试 5 次；重连成功后恢复视频流推送；超过重试次数后，THE Network_Stack SHALL 记录错误日志并进入离线存储模式。
7. IF TCP 初始连接在 30 秒内未能建立，THEN THE Network_Stack SHALL 记录连接超时日志，并进入离线存储模式等待后续重试。

---

### 需求 5：实时视频查看

**用户故事：** 作为用户，我希望通过浏览器或移动端 App 实时查看摄像头画面，以便随时监控家庭环境。

#### 验收标准

1. THE Raspberry_Pi_Server SHALL 提供基于 HTTP 的 MJPEG 视频流接口（`multipart/x-mixed-replace`），供 Web_Client 访问。
2. WHEN Web_Client 请求实时视频流，THE Raspberry_Pi_Server SHALL 在 2 秒内开始推送视频帧。
3. THE Raspberry_Pi_Server SHALL 支持 3 至 10 个 Web_Client 同时访问同一视频流，在最大并发客户端数量下每路流的帧率不低于 10fps。
4. WHERE 外网访问功能已启用，THE Raspberry_Pi_Server SHALL 要求 Web_Client 提供有效的身份认证凭据（用户名和密码），认证通过后方可访问视频流。
5. IF Web_Client 连接中断，THEN THE Raspberry_Pi_Server SHALL 在 10 秒内检测到断开并释放对应连接资源，其他已连接的 Web_Client 的视频流不受影响且帧率保持不低于 10fps。
6. IF MCU 与 Raspberry_Pi_Server 之间的视频流连接不可用，THEN THE Raspberry_Pi_Server SHALL 向请求实时视频流的 Web_Client 返回 HTTP 503 状态码及描述性错误信息。
7. WHEN 并发 Web_Client 数量达到最大值（10 个），THE Raspberry_Pi_Server SHALL 向新的连接请求返回 HTTP 503 状态码，拒绝超出上限的连接。

---

### 需求 6：历史录像查看

**用户故事：** 作为用户，我希望通过服务器界面查看和回放历史录像，以便追溯家庭环境中的历史事件。

#### 验收标准

1. THE Raspberry_Pi_Server SHALL 提供 HTTP GET API 接口（`/api/recordings`），返回 SD 卡上所有录像文件的列表，每条记录包含文件名、开始时间戳（ISO 8601 格式）和文件大小（字节）。
2. WHEN Web_Client 通过文件名请求指定录像文件，THE Raspberry_Pi_Server SHALL 以 HTTP 流式传输（`Transfer-Encoding: chunked`）方式提供该文件的下载或在线播放。
3. WHEN Web_Client 发送包含 ISO 8601 格式 `start` 和 `end` 时间戳参数的录像列表请求，THE Raspberry_Pi_Server SHALL 返回时间戳在该范围内的录像文件列表；若范围内无录像，SHALL 返回 HTTP 200 状态码及空列表。
4. IF 请求的录像文件不存在，THEN THE Raspberry_Pi_Server SHALL 返回 HTTP 404 状态码及描述性错误信息。
5. IF Web_Client 提供的时间范围参数格式不符合 ISO 8601 标准或 `start` 晚于 `end`，THEN THE Raspberry_Pi_Server SHALL 返回 HTTP 400 状态码及描述性错误信息。

---

### 需求 7：远程拍照

**用户故事：** 作为用户，我希望通过服务器界面远程触发摄像头拍照，以便在需要时获取当前时刻的静态图像（分辨率不低于 1280×720，JPEG 质量参数不低于 80/100）。

#### 验收标准

1. THE Raspberry_Pi_Server SHALL 提供 HTTP POST API 接口（`/api/snapshot`），接收 Web_Client 发送的拍照指令。
2. WHEN Raspberry_Pi_Server 收到拍照指令，THE Network_Stack SHALL 通过 TCP 连接将拍照命令转发至 MCU，命令传达延迟不超过 1 秒。
3. WHEN MCU 收到拍照命令，THE Stream_Encoder SHALL 采集并编码当前帧为 JPEG 格式（分辨率不低于 1280×720，质量参数不低于 80/100）。
4. WHEN Stream_Encoder 完成照片编码，THE File_Manager SHALL 将照片以 `SNAP_YYYYMMDD_HHMMSS.jpg` 格式存储至 SD 卡。
5. WHEN File_Manager 完成照片存储，THE Network_Stack SHALL 将照片数据上传至 Raspberry_Pi_Server。
6. WHEN Raspberry_Pi_Server 收到照片数据，THE Raspberry_Pi_Server SHALL 将照片存储至本地文件系统，并通过 HTTP 响应将照片访问 URL 返回给 Web_Client。
7. IF MCU 在收到拍照命令后 3 秒内未返回照片数据，THEN THE Raspberry_Pi_Server SHALL 向 Web_Client 返回 HTTP 504 超时错误响应。
8. IF SD 卡写入照片失败，THEN THE MCU SHALL 仍将照片数据上传至 Raspberry_Pi_Server，并在上传数据中附加 SD 卡写入失败的错误标记。

---

### 需求 8：系统配置管理

**用户故事：** 作为系统管理员，我希望能够配置系统的关键参数，以便根据实际部署环境调整系统行为。

#### 验收标准

1. WHEN MCU 上电启动，THE MCU SHALL 从 SD 卡根目录的 `config.ini` 文件中读取系统配置参数，包括 WiFi SSID、WiFi 密码、服务器 IP 地址、服务器端口、图像质量因子（50–95）和帧率目标值（1–30fps）。
2. IF `config.ini` 文件不存在或解析失败（包括参数值超出合法范围），THEN THE MCU SHALL 使用内置默认配置参数启动，并记录包含失败原因的配置加载失败日志。
3. THE Raspberry_Pi_Server SHALL 提供 HTTP GET API 接口（`/api/status`），允许 Web_Client 查询当前 MCU 运行状态，响应内容包括 WiFi 连接状态、当前帧率（fps）、SD 卡剩余空间（MB）和系统运行时间（秒）。
4. IF MCU 与 Raspberry_Pi_Server 之间的连接不可用，THEN THE Raspberry_Pi_Server SHALL 在状态响应中将 MCU 状态标记为"离线"，并返回最后一次已知状态及其时间戳。

---

### 需求 9：系统可靠性与错误恢复

**用户故事：** 作为系统管理员，我希望系统在遇到异常情况时能自动恢复，以便减少人工干预并保证监控的持续性。

#### 验收标准

1. WHEN MCU 检测到看门狗定时器超时，THE MCU SHALL 执行系统软复位，重新初始化所有外设模块（包括 Camera_Module、WiFi_Module 和 SD_Controller），并记录包含复位原因和时间戳的复位日志。
2. THE MCU SHALL 配置看门狗定时器超时时间为 5 秒，并在主循环中每 1 秒执行一次喂狗操作；IF 主循环因任意模块阻塞超过 5 秒未执行喂狗，THEN 看门狗定时器SHALL 触发系统复位。
3. IF Camera_Module 连续 10 帧采集超时（每帧超时判定阈值为 66ms），THEN THE MCU SHALL 重新初始化 Camera_Module，并记录包含连续超时帧数和时间戳的重初始化事件日志。
4. IF WiFi_Module 连续 60 秒无法维持 WiFi 连接，THEN THE Network_Stack SHALL 触发 WiFi_Module 硬件复位（拉低复位引脚至少 100ms），并重新执行完整的 WiFi 连接流程（包括重新加载配置参数）。
5. IF Camera_Module 重新初始化后连续 3 次仍无法恢复正常采集，THEN THE MCU SHALL 停止对 Camera_Module 的重试，记录不可恢复错误日志，并通过 WiFi_Module 向 Raspberry_Pi_Server 上报摄像头不可用状态。
