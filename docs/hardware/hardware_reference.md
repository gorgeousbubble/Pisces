# 硬件设计参考文档

## 系统硬件框图

```
                    ┌─────────────────────────────────────────────────────┐
                    │              主控板（自制 PCB）                       │
                    │                                                     │
  ┌──────────┐ DVP  │  ┌─────────────────────────────────────────────┐   │
  │ OV2640   │──────┼─▶│                                             │   │
  │ 摄像头   │ I2C0 │  │         NXP MK64FN1M0VLL12                  │   │
  │ 模块     │◀─────┼──│         ARM Cortex-M4 @ 120MHz              │   │
  └──────────┘      │  │         1MB Flash / 256KB RAM               │   │
                    │  │         LQFP-100                             │   │
  ┌──────────┐ UART │  │                                             │   │
  │ ESP8266  │◀─────┼──│                                             │   │
  │ WiFi模块 │──────┼─▶│                                             │   │
  └──────────┘      │  └──────────────┬──────────────────────────────┘   │
                    │                 │ SDHC(4bit)  I2C1   SWD   UART0   │
  ┌──────────┐      │                 │                                   │
  │ MicroSD  │◀─────┼─────────────────┘                                  │
  │ 卡槽     │      │                 │                                   │
  └──────────┘      │                 │ I2C1                              │
                    │          ┌──────▼──────┐                           │
                    │          │   DS3231    │  ← 可选，纽扣电池供电       │
                    │          │   RTC模块   │                           │
                    │          └─────────────┘                           │
                    │                                                     │
                    │  ┌──────────┐  ┌──────────┐  ┌──────────────────┐ │
                    │  │ 3.3V LDO │  │ 5V→3.3V  │  │ SWD 调试接口     │ │
                    │  │ AMS1117  │  │ 电源管理  │  │ J-Link/CMSIS-DAP │ │
                    │  └──────────┘  └──────────┘  └──────────────────┘ │
                    └─────────────────────────────────────────────────────┘
                                          │
                                    5V USB / DC 供电
```

---

## 元器件清单（BOM）

### 主要元器件

| 位号 | 元器件 | 型号/规格 | 封装 | 数量 | 备注 |
|------|--------|---------|------|------|------|
| U1 | 主控 MCU | NXP MK64FN1M0VLL12 | LQFP-100 | 1 | 核心芯片 |
| U2 | WiFi 模块 | ESP8266-01S | SMD 模块 | 1 | 或 ESP-WROOM-02 |
| U3 | 摄像头模块 | OV2640（含镜头） | 24pin FPC | 1 | DVP 接口，片上 JPEG |
| U4 | RTC | DS3231SN | SOIC-16 | 1 | 可选，高精度 ±2ppm |
| U5 | LDO 稳压 | AMS1117-3.3 | SOT-223 | 1 | 5V→3.3V，1A |
| U6 | LDO 稳压 | AMS1117-1.8 | SOT-223 | 1 | 3.3V→1.8V，OV2640 核心供电 |
| J1 | SD 卡槽 | MicroSD Push-Push | SMD | 1 | 带卡检测引脚 |
| J2 | 摄像头 FPC 座 | 24pin 0.5mm FPC | SMD | 1 | |
| J3 | SWD 调试接口 | 2×5 2.54mm 排针 | THT | 1 | ARM 标准 10-pin SWD |
| J4 | 串口日志接口 | 1×3 2.54mm 排针 | THT | 1 | TX/RX/GND |
| J5 | 电源输入 | Micro-USB 或 DC 5.5/2.1 | SMD/THT | 1 | 5V 输入 |
| BT1 | 纽扣电池座 | CR2032 | THT | 1 | DS3231 VBAT 供电 |
| LED1 | 状态 LED（绿） | 0805 绿色 LED | 0805 | 1 | PTB22，低有效 |
| LED2 | 错误 LED（红） | 0805 红色 LED | 0805 | 1 | PTE26，低有效 |
| SW1 | 复位按键 | 6×6mm 轻触开关 | THT | 1 | 接 RESET_b 引脚 |

### 无源元器件

| 位号 | 元器件 | 规格 | 封装 | 数量 | 用途 |
|------|--------|------|------|------|------|
| C1,C2 | 去耦电容 | 100nF 50V X7R | 0402 | 2 | MCU VDD 去耦 |
| C3–C8 | 去耦电容 | 100nF 50V X7R | 0402 | 6 | 各电源引脚去耦 |
| C9,C10 | 电源滤波 | 10μF 10V X5R | 0805 | 2 | LDO 输出滤波 |
| C11,C12 | 晶振负载 | 18pF 50V C0G | 0402 | 2 | 12MHz 晶振负载 |
| C13 | VBAT 滤波 | 100nF 10V X7R | 0402 | 1 | MCU VBAT 引脚 |
| C14 | DS3231 VBAT | 100nF 10V X7R | 0402 | 1 | DS3231 VBAT 去耦 |
| R1,R2 | I2C0 上拉 | 4.7kΩ 1/16W | 0402 | 2 | SDA/SCL（摄像头 SCCB）|
| R3,R4 | I2C1 上拉 | 4.7kΩ 1/16W | 0402 | 2 | SDA/SCL（DS3231）|
| R5 | LED1 限流 | 330Ω 1/16W | 0402 | 1 | 绿色 LED |
| R6 | LED2 限流 | 330Ω 1/16W | 0402 | 1 | 红色 LED |
| R7 | WiFi RST 上拉 | 10kΩ 1/16W | 0402 | 1 | ESP8266 RST 引脚 |
| R8 | WiFi EN 上拉 | 10kΩ 1/16W | 0402 | 1 | ESP8266 CH_PD/EN |
| R9 | RESET 上拉 | 10kΩ 1/16W | 0402 | 1 | MCU RESET_b |
| Y1 | 晶振 | 12MHz ±20ppm | 3225 SMD | 1 | MCU 主时钟 |
| Y2 | RTC 晶振 | 32.768kHz | 3215 SMD | 1 | K64 内置 RTC（若不用 DS3231）|

---

## 原理图描述

### 1. 电源电路

```
USB/DC 5V 输入
    │
    ├──[C_in 10μF]──GND
    │
    ▼
AMS1117-3.3 (U5)
    IN=5V  OUT=3.3V  ADJ=GND
    │
    ├���─[C9 10μF]──GND    ← 输出滤波
    ├──[C1 100nF]──GND   ← 高频去耦
    │
    ├──▶ VDD_3V3（MCU、ESP8266、DS3231、SD卡）
    │
    └──▶ AMS1117-1.8 (U6)
              IN=3.3V  OUT=1.8V
              │
              ├──[C10 10μF]──GND
              └──▶ DVDD_1V8（OV2640 核心供电 DVDD）

OV2640 电源：
    AVDD  = 2.8V（由 3.3V 经 100Ω 电阻降压，或专用 LDO）
    DVDD  = 1.8V（U6 输出）
    DOVDD = 2.8V（同 AVDD）
```

### 2. MCU 最小系统

```
MK64FN1M0VLL12 (U1) LQFP-100

电源引脚（每个 VDD/VDDA 引脚就近放置 100nF 去耦电容）：
    VDD    引脚 1,25,50,75,100  → 3.3V
    VDDA   引脚 12              → 3.3V（ADC 模拟电源）
    VREFH  引脚 13              → 3.3V（ADC 参考电压）
    VREFL  引脚 14              → GND
    VBAT   引脚 99              → 3.3V（经 100nF 去耦到 GND）
    VSS    引脚 26,51,76,101    → GND

复位电路：
    RESET_b（引脚 78）
        ├──[R9 10kΩ]──3.3V    ← 上拉
        └──[SW1]──GND          ← 手动复位按键

时钟：
    EXTAL0（引脚 8）──[C11 18pF]──GND
                    ──[Y1 12MHz]──
    XTAL0 （引脚 9）──[C12 18pF]──GND
                    ──[Y1 12MHz]──

    EXTAL32（引脚 97）──[Y2 32.768kHz]──
    XTAL32 （引脚 98）──[Y2 32.768kHz]──
    （若使用 DS3231，Y2 可省略）

SWD 调试接口（J3，10-pin ARM Cortex Debug）：
    Pin 1  VTREF  → 3.3V
    Pin 2  SWDIO  → PTA3（引脚 17）
    Pin 3  GND    → GND
    Pin 4  SWDCLK → PTA0（引脚 14）
    Pin 5  GND    → GND
    Pin 6  SWO    → PTA2（引脚 16，可选 trace）
    Pin 7  NC
    Pin 8  NC
    Pin 9  GND    → GND
    Pin 10 RESET  → RESET_b（引脚 78）
```

### 3. OV2640 摄像头接口

```
OV2640 FPC 24-pin（通过 J2 FPC 座连接）

电源：
    Pin 1  AVDD   → 2.8V
    Pin 2  GND    → GND
    Pin 3  DOVDD  → 2.8V
    Pin 4  AGND   → GND
    Pin 5  DVDD   → 1.8V（U6 输出）
    Pin 6  DGND   → GND

控制信号：
    Pin 7  PWDN   → GND（常低，不使用掉电模式）
    Pin 8  RESET  → 3.3V（常高，不复位）
    Pin 9  XCLK   → PTC4（引脚 55，FTM0_CH3，24MHz PWM 输出）
    Pin 10 PCLK   → PTC3（引脚 54，GPIO 输入）
    Pin 11 HREF   → PTC1（引脚 52，GPIO 输入）
    Pin 12 VSYNC  → PTC2（引脚 53，GPIO 输入，中断）

SCCB（I2C0）：
    Pin 13 SIO_C  → PTB0（引脚 35，I2C0_SCL）──[R1 4.7kΩ]──3.3V
    Pin 14 SIO_D  → PTB1（引脚 36，I2C0_SDA）──[R2 4.7kΩ]──3.3V

DVP 数据总线（8位）：
    Pin 15 D2     → PTD0（引脚 63，GPIO 输入）
    Pin 16 D3     → PTD1（引脚 64，GPIO 输入）
    Pin 17 D4     → PTD2（引脚 65，GPIO 输入）
    Pin 18 D5     → PTD3（引脚 66，GPIO 输入）
    Pin 19 D6     → PTD4（引脚 67，GPIO 输入）
    Pin 20 D7     → PTD5（引脚 68，GPIO 输入）
    Pin 21 D8     → PTD6（引脚 69，GPIO 输入）
    Pin 22 D9     → PTD7（引脚 70，GPIO 输入）
    Pin 23 GND    → GND
    Pin 24 VCC    → 2.8V
```

### 4. ESP8266 WiFi 模块接口

```
ESP8266-01S（8-pin 2×4 排针模块）

电源：
    VCC   → 3.3V（注意：ESP8266 峰值电流 ~300mA，需足够的去耦）
    GND   → GND
    ├──[C 100μF 电解]──GND   ← 大容量去耦，防止 WiFi 发射时电压跌落

控制引脚：
    CH_PD/EN → [R8 10kΩ]──3.3V，同时连接 PTA12（引脚 28，GPIO 输出）
    RST      → [R7 10kΩ]──3.3V，同时连接 PTA13（引脚 29，GPIO 输出，低有效复位）

UART（AT 指令）：
    TXD  → PTC3（引脚 54，UART1_RX）  ← ESP8266 发送，MCU 接收
    RXD  → PTC4（引脚 55，UART1_TX）  ← ESP8266 接收，MCU 发送

注意：PTC3/PTC4 与摄像头 PCLK/XCLK 复用，需通过软件配置 MUX 切换。
实际布线建议将 WiFi UART 改用 UART4（PTE24/PTE25）避免冲突。
（board.h 中已预留，可根据实际 PCB 调整）
```

### 5. SD 卡接口

```
MicroSD 卡槽（J1，Push-Push 带 CD 引脚）

电源：
    VDD  → 3.3V
    VSS  → GND
    ├──[C 100nF]──GND   ← 去耦

SDHC 4位总线：
    CLK  → PTE2（引脚 3，SDHC_DCLK）
    CMD  → PTE3（引脚 4，SDHC_CMD）──[R 47kΩ]──3.3V（上拉）
    DAT0 → PTE1（引脚 2，SDHC_D0） ──[R 47kΩ]──3.3V
    DAT1 → PTE0（引脚 1，SDHC_D1） ──[R 47kΩ]──3.3V
    DAT2 → PTD13（引脚 86，SDHC_D2）──[R 47kΩ]──3.3V
    DAT3 → PTD12（引脚 85，SDHC_D3）──[R 47kΩ]──3.3V

卡检测：
    CD   → PTE6（引脚 7，GPIO 输入）──[R 10kΩ]──3.3V（上拉）
           卡插入时 CD 拉低
```

### 6. DS3231 RTC 接口（可选）

```
DS3231SN（SOIC-16）

电源：
    VCC  → 3.3V
    GND  → GND
    VBAT → BT1（CR2032 纽扣电池正极）──[D 1N4148]──3.3V（防反充）
    ├──[C14 100nF]──GND

I2C1（独立总线，不与摄像头共用）：
    SCL  → PTC10（引脚 61，I2C1_SCL）──[R3 4.7kΩ]──3.3V
    SDA  → PTC11（引脚 62，I2C1_SDA）──[R4 4.7kΩ]──3.3V

中断/方波输出（可选）：
    INT/SQW → 悬空（固件中不使用）
    32kHz   → 悬空
```

### 7. 状态指示 LED

```
LED1（绿色，系统状态）：
    PTB22（引脚 47）──[R5 330Ω]──LED1（绿）──GND
    低电平点亮

LED2（红色，错误状态）：
    PTE26（引脚 11）──[R6 330Ω]──LED2（红）──GND
    低电平点亮
```

### 8. 串口日志接口

```
J4（1×3 2.54mm 排针）：
    Pin 1  GND
    Pin 2  TX  → PTA14（引脚 30，UART0_TX）
    Pin 3  RX  → PTA15（引脚 31，UART0_RX）

波特率：115200 8N1
```

---

## 引脚分配完整表

| K64 引脚号 | 引脚名 | 功能 | 连接目标 | 方向 |
|-----------|--------|------|---------|------|
| 1 | PTA0 | SWD_CLK | J3 Pin4 | I/O |
| 2 | PTA1 | — | 悬空 | — |
| 3 | PTA2 | SWO | J3 Pin6 | O |
| 4 | PTA3 | SWD_DIO | J3 Pin2 | I/O |
| 11 | PTE26 | GPIO_OUT | LED2（红）| O |
| 14 | PTA0 | UART0_RX | J4 Pin3 | I |
| 17 | PTA3 | UART0_TX | J4 Pin2 | O |
| 28 | PTA12 | GPIO_OUT | ESP8266 EN | O |
| 29 | PTA13 | GPIO_OUT | ESP8266 RST | O |
| 30 | PTA14 | UART0_TX | J4 Pin2 | O |
| 31 | PTA15 | UART0_RX | J4 Pin3 | I |
| 35 | PTB0 | I2C0_SCL | OV2640 SIO_C | O |
| 36 | PTB1 | I2C0_SDA | OV2640 SIO_D | I/O |
| 47 | PTB22 | GPIO_OUT | LED1（绿）| O |
| 52 | PTC1 | GPIO_IN | OV2640 HREF | I |
| 53 | PTC2 | GPIO_IN/IRQ | OV2640 VSYNC | I |
| 54 | PTC3 | UART1_RX | ESP8266 TXD | I |
| 55 | PTC4 | FTM0_CH3/UART1_TX | OV2640 XCLK / ESP8266 RXD | O |
| 61 | PTC10 | I2C1_SCL | DS3231 SCL | O |
| 62 | PTC11 | I2C1_SDA | DS3231 SDA | I/O |
| 63 | PTD0 | GPIO_IN | OV2640 D2 | I |
| 64 | PTD1 | GPIO_IN | OV2640 D3 | I |
| 65 | PTD2 | GPIO_IN | OV2640 D4 | I |
| 66 | PTD3 | GPIO_IN | OV2640 D5 | I |
| 67 | PTD4 | GPIO_IN | OV2640 D6 | I |
| 68 | PTD5 | GPIO_IN | OV2640 D7 | I |
| 69 | PTD6 | GPIO_IN | OV2640 D8 | I |
| 70 | PTD7 | GPIO_IN | OV2640 D9 | I |
| 85 | PTD12 | SDHC_D3 | SD DAT3 | I/O |
| 86 | PTD13 | SDHC_D2 | SD DAT2 | I/O |
| 1 | PTE0 | SDHC_D1 | SD DAT1 | I/O |
| 2 | PTE1 | SDHC_D0 | SD DAT0 | I/O |
| 3 | PTE2 | SDHC_DCLK | SD CLK | O |
| 4 | PTE3 | SDHC_CMD | SD CMD | I/O |
| 7 | PTE6 | GPIO_IN | SD CD | I |
| 78 | RESET_b | RESET | SW1 / J3 Pin10 | I |

---

## PCB 布局建议

### 关键布局规则

**电源层**
- 使用 4 层板：顶层信号、内层 GND、内层 3.3V、底层信号
- 每个 VDD 引脚就近放置 100nF 去耦电容，走线尽量短（< 3mm）
- AMS1117 LDO 散热焊盘连接到 GND 铜皮

**高速信号（SDHC）**
- SD 卡 CLK/CMD/DAT0–3 走线等长（误差 < 5mm）
- 走线宽度 ≥ 0.2mm，避免过孔
- 远离 WiFi 天线区域

**DVP 摄像头总线**
- D0–D7、PCLK、HREF、VSYNC 等长（误差 < 10mm）
- 走线宽度 0.15mm，差分对间距 0.15mm
- FPC 座靠近板边，减少走线长度

**WiFi 模块**
- ESP8266 模块天线端悬空于 PCB 边缘外（或开窗）
- 天线下方禁止铺铜
- 模块供电加 100μF 电解电容（防发射时电压跌落）

**RTC（DS3231）**
- 靠近 MCU 放置，I2C 走线 < 50mm
- 纽扣电池座放置在板边，方便更换
- VBAT 走线远离高频信号

**晶振**
- 12MHz 晶振紧靠 MCU EXTAL0/XTAL0 引脚
- 晶振下方禁止走信号线，铺 GND 屏蔽
- 负载电容就近放置

### 推荐板尺寸

- 主控板：80mm × 60mm（4 层板）
- 摄像头模块通过 FPC 排线连接，可独立安装

---

## 调试接口说明

### SWD 调试（J3）

使用 J-Link、CMSIS-DAP 或 OpenSDA 调试器连接 J3：

```
J3 引脚定义（10-pin 2×5 2.54mm，ARM Cortex-M 标准）：
┌───┬───┐
│ 1 │ 2 │  1=VTREF(3.3V)  2=SWDIO
│ 3 │ 4 │  3=GND          4=SWDCLK
│ 5 │ 6 │  5=GND          6=SWO
│ 7 │ 8 │  7=NC           8=NC
│ 9 │10 │  9=GND          10=RESET
└───┴───┘
```

### 串口日志（J4）

```
J4 引脚定义（1×3 2.54mm）：
Pin 1 = GND
Pin 2 = TX（MCU 发送，接 USB-TTL 的 RX）
Pin 3 = RX（MCU 接收，接 USB-TTL 的 TX）

波特率：115200 8N1
```

---

## 注意事项

1. **PTC3/PTC4 复用冲突**：摄像头 PCLK（PTC3）与 WiFi UART1_RX（PTC3）、摄像头 XCLK（PTC4）与 WiFi UART1_TX（PTC4）存在引脚复用冲突。建议在 PCB 上将 WiFi UART 改接 UART4（PTE24=TX，PTE25=RX），并相应修改 `board.h`。

2. **ESP8266 供电**：ESP8266 在 WiFi 发射时峰值电流可达 300mA，必须在模块 VCC 引脚就近放置 100μF 电解电容，否则可能导致 MCU 复位。

3. **OV2640 AVDD**：OV2640 的 AVDD 需要 2.8V，可用 3.3V 经 100Ω 电阻 + 100μF 电容组成 RC 滤波，或使用专用 2.8V LDO（如 MIC5219）。

4. **SD 卡热插拔**：若需要支持热插拔，CD 引脚需要硬件去抖（RC 滤波：10kΩ + 100nF），并在固件中处理插拔中断。

5. **DS3231 防反充**：CR2032 纽扣电池与 VCC 之间必须串联二极管（1N4148 或肖特基），防止系统供电时对电池充电。
