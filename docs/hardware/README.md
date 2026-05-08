# 硬件设计文档索引

## 文件说明

| 文件 | 说明 |
|------|------|
| [hardware_reference.md](./hardware_reference.md) | 完整硬件设计参考，含原理图描述、BOM、PCB 布局建议 |
| [pisces_netlist.net](./pisces_netlist.net) | KiCad 网表文件，可导入 KiCad 生成原理图和 PCB |

## 快速参考

### 核心芯片

| 芯片 | 封装 | 关键参数 |
|------|------|---------|
| MK64FN1M0VLL12 | LQFP-100 | 120MHz, 1MB Flash, 256KB RAM |
| OV2640 | 24-pin FPC | 2MP, DVP, 片上 JPEG, VGA@30fps |
| ESP8266-01S | 模块 | 802.11 b/g/n, AT 指令, UART |
| DS3231SN | SOIC-16 | ±2ppm RTC, I2C, VBAT 备份（可选）|
| AMS1117-3.3 | SOT-223 | 5V→3.3V, 1A LDO |
| AMS1117-1.8 | SOT-223 | 3.3V→1.8V, 1A LDO（OV2640 DVDD）|

### 关键引脚速查

```
摄像头 DVP：PTD0–PTD7（D2–D9），PTC1(HREF)，PTC2(VSYNC)，PTC3(PCLK)，PTC4(XCLK)
摄像头 SCCB：PTB0(SCL)，PTB1(SDA)
WiFi UART：PTC4(TX)，PTC3(RX)，PTA12(EN)，PTA13(RST)
SD 卡 SDHC：PTE0–PTE3，PTD12–PTD13，PTE6(CD)
RTC I2C：PTC10(SCL)，PTC11(SDA)
SWD：PTA0(CLK)，PTA3(DIO)
日志 UART：PTA14(TX)，PTA15(RX)
LED：PTB22(绿)，PTE26(红)
```

### 电源需求

| 电源 | 电压 | 最大电流 | 用途 |
|------|------|---------|------|
| 输入 | 5V | 1A | USB/DC 输入 |
| VDD_3V3 | 3.3V | 800mA | MCU、WiFi、SD、DS3231 |
| VDD_1V8 | 1.8V | 200mA | OV2640 DVDD |
| VDD_2V8 | 2.8V | 100mA | OV2640 AVDD/DOVDD |
| VBAT | 3V（CR2032）| μA 级 | DS3231 掉电保持 |

## 使用 KiCad 导入网表

1. 安装 [KiCad 7.0+](https://www.kicad.org/)
2. 新建工程，打开原理图编辑器
3. `File → Import → Netlist`，选择 `pisces_netlist.net`
4. 根据网表自动放置元器件，手动连线
5. 完成原理图后，使用 `Tools → Update PCB from Schematic` 生成 PCB

> 提示：网表中已包含推荐封装（KiCad 标准库），导入后可直接使用。
