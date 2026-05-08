# K64 IP Camera Firmware

基于 NXP MK64FN1M0VLL12 的网络摄像头监控系统固件。

## 工程结构

```
firmware/
├── src/                    # 全部 C 源码（Keil/IAR 共享）
│   ├── app/                # 应用层
│   ├── service/            # 服务层
│   ├── driver/             # 驱动层
│   └── hal/                # 硬件抽象层（板级初始化）
├── include/                # 公共头文件
├── third_party/            # 第三方库
│   ├── FreeRTOS/
│   ├── FatFs/
│   └── KSDK/               # NXP KSDK 2.x 精简版头文件
├── ide/
│   ├── keil/               # Keil5 MDK 工程文件 (.uvprojx)
│   └── iar/                # IAR EWARM 工程文件 (.ewp/.eww)
└── docs/                   # 硬件相关文档
```

## 构建环境

| IDE | 版本要求 | 编译器 |
|-----|---------|--------|
| Keil MDK | ≥ 5.38 | ARM Compiler 6 (armclang) |
| IAR EWARM | ≥ 9.30 | IAR C/C++ Compiler for ARM |

## 依赖

- NXP KSDK 2.x（需单独安装，路径配置见各 IDE 工程）
- FreeRTOS 10.5.1（已内嵌于 third_party/）
- FatFs R0.15（已内嵌于 third_party/）

## 调试接口

- SWD（推荐）：SWDIO=PTA3, SWDCLK=PTA0
- JTAG：可选
- 串口日志：UART0，115200 8N1，PTA14(TX)/PTA15(RX)
