# 第三方库说明

本目录存放所有第三方依赖库，需手动下载后放置到对应目录。

## 目录结构

```
third_party/
├── FreeRTOS/          # FreeRTOS 10.5.1
├── FatFs/             # FatFs R0.15
└── KSDK/              # NXP KSDK 2.x (MCUXpresso SDK)
```

---

## 1. FreeRTOS 10.5.1

**下载地址：** https://github.com/FreeRTOS/FreeRTOS-Kernel/releases/tag/V10.5.1

**需要的文件：**
```
FreeRTOS/
├── include/           # 全部头文件
├── tasks.c
├── queue.c
├── list.c
├── timers.c
├── event_groups.c
├── stream_buffer.c
└── portable/
    ├── MemMang/
    │   └── heap_4.c   # 使用 heap_4 内存管理
    ├── RVDS/
    │   └── ARM_CM4F/
    │       └── port.c         # Keil 使用
    └── IAR/
        └── ARM_CM4F/
            ├── port.c         # IAR 使用
            └── portasm.s      # IAR 汇编端口文件
```

---

## 2. FatFs R0.15

**下载地址：** http://elm-chan.org/fsw/ff/00index_e.html

**需要的文件：**
```
FatFs/
└── source/
    ├── ff.h
    ├── ff.c
    ├── ffconf.h       # 配置文件（见下方配置说明）
    ├── diskio.h
    ├── diskio.c       # 底层驱动接口（需实现 SDHC 版本）
    ├── ffsystem.c
    └── ffunicode.c
```

**ffconf.h 关键配置：**
```c
#define FF_FS_READONLY   0      // 读写模式
#define FF_FS_MINIMIZE   0      // 完整功能
#define FF_USE_STRFUNC   1      // 启用字符串函数（f_gets 等）
#define FF_USE_FIND      0
#define FF_USE_MKFS      1      // 启用格式化
#define FF_USE_FASTSEEK  0
#define FF_USE_EXPAND    0
#define FF_USE_CHMOD     0
#define FF_USE_LABEL     0
#define FF_USE_FORWARD   0
#define FF_USE_LFN       1      // 启用长文件名
#define FF_MAX_LFN       255
#define FF_LFN_UNICODE   0
#define FF_LFN_BUF       255
#define FF_SFN_BUF       12
#define FF_FS_RPATH      0
#define FF_VOLUMES       1      // 单卷
#define FF_STR_VOLUME_ID 0
#define FF_MULTI_PARTITION 0
#define FF_MIN_SS        512
#define FF_MAX_SS        512
#define FF_FS_EXFAT      0
#define FF_FS_NORTC      0      // 使用 RTC 时间戳
#define FF_FS_NOFSINFO   0
#define FF_FS_LOCK       4      // 支持 4 个同时打开的文件
#define FF_FS_REENTRANT  1      // 线程安全（需实现 ff_mutex_*）
#define FF_SYNC_t        SemaphoreHandle_t
```

---

## 3. NXP KSDK 2.x (MCUXpresso SDK for MK64F12)

**下载地址：** https://mcuxpresso.nxp.com/en/select

选择：
- Board: FRDM-K64F（或 Custom Board）
- Device: MK64FN1M0VLL12
- SDK Version: 2.14.x（最新稳定版）
- Components: 勾选 FreeRTOS、SDHC、UART、I2C、GPIO、FTM、WDOG

**需要的文件（精简版）：**
```
KSDK/
├── CMSIS/
│   └── Include/           # ARM CMSIS 头文件（core_cm4.h 等）
└── devices/
    └── MK64F12/
        ├── MK64F12.h          # 设备头文件（寄存器定义）
        ├── system_MK64F12.h
        ├── system_MK64F12.c
        ├── fsl_device_registers.h
        ├── arm/
        │   └── startup_MK64F12.s   # Keil 启动文件
        ├── iar/
        │   └── startup_MK64F12.s   # IAR 启动文件
        └── drivers/
            ├── fsl_clock.h / fsl_clock.c
            ├── fsl_uart.h  / fsl_uart.c
            ├── fsl_i2c.h   / fsl_i2c.c
            ├── fsl_gpio.h  / fsl_gpio.c
            ├── fsl_port.h
            ├── fsl_ftm.h   / fsl_ftm.c
            ├── fsl_wdog.h  / fsl_wdog.c
            ├── fsl_sdhc.h  / fsl_sdhc.c
            └── fsl_common.h / fsl_common.c
```

---

## 快速安装步骤

1. 安装 MCUXpresso SDK Builder，生成 SDK 包并解压到 `KSDK/`
2. 从 FreeRTOS GitHub 下载内核源码，按上述结构放置到 `FreeRTOS/`
3. 从 elm-chan 下载 FatFs，按上述结构放置到 `FatFs/`，修改 `ffconf.h`
4. 用 Keil5 打开 `ide/keil/k64_ipcam.uvprojx`，或用 IAR 打开 `ide/iar/k64_ipcam.eww`
5. 编译，确认无错误后烧录调试
