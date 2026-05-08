/**
 * @file board.c
 * @brief 板级初始化实现 - MK64FN1M0VLL12
 */

#include "board.h"
#include "fsl_clock.h"
#include "fsl_port.h"
#include "fsl_gpio.h"
#include "fsl_uart.h"
#include "log.h"

/* -----------------------------------------------------------------------
 * 时钟配置（120MHz，使用外部 12MHz 晶振 + PLL）
 * ----------------------------------------------------------------------- */
static const mcg_config_t s_mcgConfig = {
    .mcgMode       = kMCG_ModePEE,
    .irclkEnableMode = 0U,
    .ircs          = kMCG_IrcSlow,
    .fcrdiv        = 0U,
    .frdiv         = 4U,
    .drs           = kMCG_DrsLow,
    .dmx32         = kMCG_Dmx32Default,
    .oscsel        = kMCG_OscselOsc,
    .pll0Config    = {
        .enableMode = 0U,
        .prdiv      = 0x03U,   /* /4  -> 3MHz */
        .vdiv       = 0x18U,   /* x40 -> 120MHz */
    },
};

static const sim_clock_config_t s_simConfig = {
    .pllFllSel    = 1U,        /* PLLFLLSEL: PLL */
    .pllFllDiv    = 0U,
    .pllFllFrac   = 0U,
    .er32kSrc     = 3U,        /* ERCLK32K: LPO */
    .clkdiv1      = 0x01140000U, /* OUTDIV1=/1, OUTDIV2=/2, OUTDIV4=/5 */
};

static const osc_config_t s_oscConfig = {
    .freq         = 12000000U,
    .capLoad      = 0U,
    .workMode     = kOSC_ModeOscLowPower,
    .oscerConfig  = {
        .enableMode = kOSC_ErClkEnable,
        .erclkDiv   = 0U,
    },
};

/* -----------------------------------------------------------------------
 * BOARD_InitClocks
 * ----------------------------------------------------------------------- */
void BOARD_InitClocks(void)
{
    CLOCK_SetSimSafeDivs();
    CLOCK_InitOsc0(&s_oscConfig);
    CLOCK_SetXtal0Freq(12000000U);
    CLOCK_BootToFeeMode(kMCG_OscselOsc, 4U, kMCG_Dmx32Default, kMCG_DrsLow, NULL);
    CLOCK_SetInternalRefClkConfig(0U, kMCG_IrcSlow, 0U);
    CLOCK_BootToPeeMode(kMCG_OscselOsc, kMCG_PllClkSelPll0, &s_mcgConfig.pll0Config);
    CLOCK_SetSimConfig(&s_simConfig);
    SystemCoreClock = BOARD_CORE_CLOCK_HZ;
}

/* -----------------------------------------------------------------------
 * BOARD_InitPins
 * ----------------------------------------------------------------------- */
void BOARD_InitPins(void)
{
    /* 使能所有用到的端口时钟 */
    CLOCK_EnableClock(kCLOCK_PortA);
    CLOCK_EnableClock(kCLOCK_PortB);
    CLOCK_EnableClock(kCLOCK_PortC);
    CLOCK_EnableClock(kCLOCK_PortD);
    CLOCK_EnableClock(kCLOCK_PortE);

    /* --- 串口日志 UART0 (PTA14/PTA15) --- */
    PORT_SetPinMux(LOG_UART_TX_PORT, LOG_UART_TX_PIN, LOG_UART_TX_MUX);
    PORT_SetPinMux(LOG_UART_RX_PORT, LOG_UART_RX_PIN, LOG_UART_RX_MUX);

    /* --- WiFi UART4 (PTE24/PTE25) --- */
    PORT_SetPinMux(WIFI_UART_TX_PORT, WIFI_UART_TX_PIN, WIFI_UART_TX_MUX);
    PORT_SetPinMux(WIFI_UART_RX_PORT, WIFI_UART_RX_PIN, WIFI_UART_RX_MUX);

    /* --- WiFi 控制引脚 (PTA12/PTA13) --- */
    {
        gpio_pin_config_t out_cfg = {kGPIO_DigitalOutput, 1U}; /* 默认高电平 */
        PORT_SetPinMux(WIFI_RST_PORT, WIFI_RST_PIN, kPORT_MuxAsGpio);
        PORT_SetPinMux(WIFI_EN_PORT,  WIFI_EN_PIN,  kPORT_MuxAsGpio);
        GPIO_PinInit(WIFI_RST_GPIO, WIFI_RST_PIN, &out_cfg);
        GPIO_PinInit(WIFI_EN_GPIO,  WIFI_EN_PIN,  &out_cfg);
    }

    /* --- 摄像头 I2C0 (PTB0/PTB1) --- */
    {
        port_pin_config_t i2c_cfg = {
            .pullSelect       = kPORT_PullUp,
            .slewRate         = kPORT_FastSlewRate,
            .passiveFilterEnable = kPORT_PassiveFilterDisable,
            .openDrainEnable  = kPORT_OpenDrainEnable,
            .driveStrength    = kPORT_HighDriveStrength,
            .mux              = CAM_I2C_MUX,
            .lockRegister     = kPORT_UnlockRegister,
        };
        PORT_SetPinConfig(CAM_I2C_SCL_PORT, CAM_I2C_SCL_PIN, &i2c_cfg);
        PORT_SetPinConfig(CAM_I2C_SDA_PORT, CAM_I2C_SDA_PIN, &i2c_cfg);
    }

    /* --- 摄像头 DVP 数据总线 PTD0-PTD7 --- */
    for (uint32_t i = 0U; i < 8U; i++) {
        PORT_SetPinMux(CAM_DATA_PORT, i, kPORT_MuxAsGpio);
    }
    /* 配置为输入 */
    GPIO_PortInputEnable(CAM_DATA_GPIO, CAM_DATA_MASK);

    /* --- 摄像头控制信号 --- */
    PORT_SetPinMux(CAM_PCLK_PORT,  CAM_PCLK_PIN,  kPORT_MuxAsGpio);
    PORT_SetPinMux(CAM_VSYNC_PORT, CAM_VSYNC_PIN, kPORT_MuxAsGpio);
    PORT_SetPinMux(CAM_HREF_PORT,  CAM_HREF_PIN,  kPORT_MuxAsGpio);
    /* XCLK 由 FTM0_CH3 输出，在 cam_driver 中配置 */
    PORT_SetPinMux(CAM_XCLK_PORT,  CAM_XCLK_PIN,  kPORT_MuxAlt4);

    /* --- SD 卡 SDHC --- */
    {
        port_pin_config_t sd_cfg = {
            .pullSelect       = kPORT_PullUp,
            .slewRate         = kPORT_FastSlewRate,
            .passiveFilterEnable = kPORT_PassiveFilterDisable,
            .openDrainEnable  = kPORT_OpenDrainDisable,
            .driveStrength    = kPORT_HighDriveStrength,
            .mux              = SD_SDHC_MUX,
            .lockRegister     = kPORT_UnlockRegister,
        };
        PORT_SetPinConfig(SD_CLK_PORT, SD_CLK_PIN, &sd_cfg);
        PORT_SetPinConfig(SD_CMD_PORT, SD_CMD_PIN, &sd_cfg);
        PORT_SetPinConfig(SD_D0_PORT,  SD_D0_PIN,  &sd_cfg);
        PORT_SetPinConfig(SD_D1_PORT,  SD_D1_PIN,  &sd_cfg);
        PORT_SetPinConfig(SD_D2_PORT,  SD_D2_PIN,  &sd_cfg);
        PORT_SetPinConfig(SD_D3_PORT,  SD_D3_PIN,  &sd_cfg);
    }
    /* SD 卡检测引脚（输入，上拉） */
    {
        port_pin_config_t cd_cfg = {
            .pullSelect       = kPORT_PullUp,
            .slewRate         = kPORT_FastSlewRate,
            .passiveFilterEnable = kPORT_PassiveFilterDisable,
            .openDrainEnable  = kPORT_OpenDrainDisable,
            .driveStrength    = kPORT_LowDriveStrength,
            .mux              = kPORT_MuxAsGpio,
            .lockRegister     = kPORT_UnlockRegister,
        };
        PORT_SetPinConfig(SD_CD_PORT, SD_CD_PIN, &cd_cfg);
        gpio_pin_config_t in_cfg = {kGPIO_DigitalInput, 0U};
        GPIO_PinInit(SD_CD_GPIO, SD_CD_PIN, &in_cfg);
    }

    /* --- 状态 LED --- */
    {
        gpio_pin_config_t led_cfg = {kGPIO_DigitalOutput, 1U}; /* 默认熄灭（高电平） */
        PORT_SetPinMux(LED_STATUS_PORT, LED_STATUS_PIN, kPORT_MuxAsGpio);
        PORT_SetPinMux(LED_ERROR_PORT,  LED_ERROR_PIN,  kPORT_MuxAsGpio);
        GPIO_PinInit(LED_STATUS_GPIO, LED_STATUS_PIN, &led_cfg);
        GPIO_PinInit(LED_ERROR_GPIO,  LED_ERROR_PIN,  &led_cfg);
    }
}

/* -----------------------------------------------------------------------
 * BOARD_InitDebugConsole
 * ----------------------------------------------------------------------- */
void BOARD_InitDebugConsole(void)
{
    /* 由 log_init() 负责初始化 UART0，此处仅作占位 */
    (void)0;
}
