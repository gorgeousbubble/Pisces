#ifndef BOARD_H
#define BOARD_H

/**
 * @file board.h
 * @brief 板级硬件定义 - MK64FN1M0VLL12
 *
 * 引脚分配与外设配置宏定义，供所有模块引用。
 */

#include "MK64F12.h"

/* -----------------------------------------------------------------------
 * 系统时钟
 * ----------------------------------------------------------------------- */
#define BOARD_CORE_CLOCK_HZ     120000000UL   /**< 核心时钟 120MHz */
#define BOARD_BUS_CLOCK_HZ       60000000UL   /**< 总线时钟  60MHz */
#define BOARD_FLASH_CLOCK_HZ     24000000UL   /**< Flash时钟 24MHz */

/* -----------------------------------------------------------------------
 * 串口日志 (UART0)
 * ----------------------------------------------------------------------- */
#define LOG_UART                 UART0
#define LOG_UART_CLKSRC          kCLOCK_CoreSysClk
#define LOG_UART_BAUDRATE        115200U
#define LOG_UART_TX_PORT         PORTA
#define LOG_UART_TX_PIN          14U           /**< PTA14 - UART0_TX */
#define LOG_UART_RX_PORT         PORTA
#define LOG_UART_RX_PIN          15U           /**< PTA15 - UART0_RX */
#define LOG_UART_TX_MUX          kPORT_MuxAlt3
#define LOG_UART_RX_MUX          kPORT_MuxAlt3

/* -----------------------------------------------------------------------
 * 摄像头 OV2640 - DVP 接口
 * ----------------------------------------------------------------------- */
/* 数据总线 D0-D7 -> PTD0-PTD7 */
#define CAM_DATA_PORT            PORTD
#define CAM_DATA_GPIO            PTD
#define CAM_DATA_MASK            0x00FFU

/* 控制信号 */
#define CAM_PCLK_PORT            PORTC
#define CAM_PCLK_PIN             3U            /**< PTC3  - 像素时钟输入 */
#define CAM_VSYNC_PORT           PORTC
#define CAM_VSYNC_PIN            2U            /**< PTC2  - 帧同步输入 */
#define CAM_HREF_PORT            PORTC
#define CAM_HREF_PIN             1U            /**< PTC1  - 行同步输入 */
#define CAM_XCLK_PORT            PORTC
#define CAM_XCLK_PIN             4U            /**< PTC4  - 主时钟输出(FTM0_CH3, 24MHz) */
#define CAM_XCLK_FTM             FTM0
#define CAM_XCLK_FTM_CHANNEL     3U

/* SCCB (I2C0) */
#define CAM_I2C                  I2C0
#define CAM_I2C_SCL_PORT         PORTB
#define CAM_I2C_SCL_PIN          0U            /**< PTB0  - I2C0_SCL */
#define CAM_I2C_SDA_PORT         PORTB
#define CAM_I2C_SDA_PIN          1U            /**< PTB1  - I2C0_SDA */
#define CAM_I2C_MUX              kPORT_MuxAlt2
#define CAM_I2C_BAUDRATE         400000U       /**< 400kHz Fast Mode */
#define CAM_OV2640_ADDR          0x30U         /**< OV2640 SCCB 地址 */

/* -----------------------------------------------------------------------
 * WiFi 模块 ESP8266 (UART4)
 *
 * 修复：原 UART1 使用 PTC3/PTC4，与摄像头 PCLK/XCLK 冲突。
 * 改用 UART4（PTE24=TX，PTE25=RX），与摄像头总线完全隔离。
 * PCB 布线时将 ESP8266 RXD/TXD 连接到 PTE24/PTE25。
 * ----------------------------------------------------------------------- */
#define WIFI_UART                UART4
#define WIFI_UART_CLKSRC         kCLOCK_BusClk
#define WIFI_UART_BAUDRATE       115200U
#define WIFI_UART_TX_PORT        PORTE
#define WIFI_UART_TX_PIN         24U           /**< PTE24 - UART4_TX (原 PTC4 冲突已修复) */
#define WIFI_UART_RX_PORT        PORTE
#define WIFI_UART_RX_PIN         25U           /**< PTE25 - UART4_RX (原 PTC3 冲突已修复) */
#define WIFI_UART_TX_MUX         kPORT_MuxAlt3
#define WIFI_UART_RX_MUX         kPORT_MuxAlt3
#define WIFI_UART_IRQ            UART4_RX_TX_IRQn
#define WIFI_UART_RX_BUF_SIZE    512U

/* WiFi 控制引脚 */
#define WIFI_RST_PORT            PORTA
#define WIFI_RST_GPIO            PTA
#define WIFI_RST_PIN             13U           /**< PTA13 - WiFi RST (低有效) */
#define WIFI_EN_PORT             PORTA
#define WIFI_EN_GPIO             PTA
#define WIFI_EN_PIN              12U           /**< PTA12 - WiFi EN */

/* -----------------------------------------------------------------------
 * SD 卡 (SDHC)
 * ----------------------------------------------------------------------- */
#define SD_SDHC                  SDHC
#define SD_CLK_PORT              PORTE
#define SD_CLK_PIN               2U            /**< PTE2  - SDHC_DCLK */
#define SD_CMD_PORT              PORTE
#define SD_CMD_PIN               3U            /**< PTE3  - SDHC_CMD  */
#define SD_D0_PORT               PORTE
#define SD_D0_PIN                1U            /**< PTE1  - SDHC_D0   */
#define SD_D1_PORT               PORTE
#define SD_D1_PIN                0U            /**< PTE0  - SDHC_D1   */
#define SD_D2_PORT               PORTD
#define SD_D2_PIN                13U           /**< PTD13 - SDHC_D2   */
#define SD_D3_PORT               PORTD
#define SD_D3_PIN                12U           /**< PTD12 - SDHC_D3   */
#define SD_CD_PORT               PORTE
#define SD_CD_GPIO               PTE
#define SD_CD_PIN                6U            /**< PTE6  - 卡检测 (低=插入) */
#define SD_SDHC_MUX              kPORT_MuxAlt4

/* -----------------------------------------------------------------------
 * 状态 LED
 * ----------------------------------------------------------------------- */
#define LED_STATUS_PORT          PORTB
#define LED_STATUS_GPIO          PTB
#define LED_STATUS_PIN           22U           /**< PTB22 - 绿色状态LED (低有效) */
#define LED_ERROR_PORT           PORTE
#define LED_ERROR_GPIO           PTE
#define LED_ERROR_PIN            26U           /**< PTE26 - 红色错误LED (低有效) */

/* LED 操作宏 */
#define LED_STATUS_ON()          GPIO_PinWrite(LED_STATUS_GPIO, LED_STATUS_PIN, 0U)
#define LED_STATUS_OFF()         GPIO_PinWrite(LED_STATUS_GPIO, LED_STATUS_PIN, 1U)
#define LED_STATUS_TOGGLE()      GPIO_PortToggle(LED_STATUS_GPIO, 1U << LED_STATUS_PIN)
#define LED_ERROR_ON()           GPIO_PinWrite(LED_ERROR_GPIO, LED_ERROR_PIN, 0U)
#define LED_ERROR_OFF()          GPIO_PinWrite(LED_ERROR_GPIO, LED_ERROR_PIN, 1U)

/* -----------------------------------------------------------------------
 * 看门狗
 * ----------------------------------------------------------------------- */
#define WDG_TIMEOUT_MS           5000U         /**< 看门狗超时 5 秒 */
#define WDG_FEED_INTERVAL_MS     1000U         /**< 喂狗间隔 1 秒 */

/* -----------------------------------------------------------------------
 * 函数声明
 * ----------------------------------------------------------------------- */
void BOARD_InitPins(void);
void BOARD_InitClocks(void);
void BOARD_InitDebugConsole(void);

#endif /* BOARD_H */
