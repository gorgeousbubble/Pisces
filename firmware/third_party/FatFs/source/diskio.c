/**
 * @file diskio.c
 * @brief FatFs 底层磁盘 I/O 驱动 - MK64 SDHC 适配
 *
 * 将 FatFs 的 disk_* 接口映射到 NXP KSDK SDHC 驱动。
 * 卷号 0 对应 SD 卡（SDHC 控制器）。
 *
 * 依赖：
 *   - NXP KSDK fsl_sdhc.c / fsl_sd.c
 *   - FreeRTOS（用于延时和互斥）
 */

#include "ff.h"
#include "diskio.h"
#include "board.h"
#include "rtc_driver.h"
#include "fsl_sdhc.h"
#include "fsl_gpio.h"
#include "fsl_clock.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>

/* -----------------------------------------------------------------------
 * SD 卡操作常量
 * ----------------------------------------------------------------------- */
#define SD_SECTOR_SIZE          512U
#define SD_INIT_TIMEOUT_MS      2000U
#define SD_RW_TIMEOUT_MS        500U

/* SDHC ADMA2 描述符表（每次最多传输 32 个扇区 = 16KB） */
#define ADMA2_TABLE_ENTRIES     4U

/* -----------------------------------------------------------------------
 * SD 卡状态
 * ----------------------------------------------------------------------- */
typedef struct {
    bool     initialized;
    bool     card_present;
    uint32_t sector_count;   /**< 卡总扇区数 */
    uint32_t rca;            /**< 相对卡地址 */
    bool     high_capacity;  /**< SDHC/SDXC = true，SDSC = false */
} sd_state_t;

static sd_state_t s_sd;

/* ADMA2 描述符表（4 字节对齐） */
static sdhc_adma2_descriptor_t s_adma2_table[ADMA2_TABLE_ENTRIES]
    __attribute__((aligned(4)));

/* -----------------------------------------------------------------------
 * 私有：等待 SDHC 命令完成
 * ----------------------------------------------------------------------- */
static bool sdhc_wait_cmd_done(uint32_t timeout_ms)
{
    uint32_t start = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    while (true) {
        uint32_t flags = SDHC_GetInterruptStatusFlags(SD_SDHC);
        if (flags & kSDHC_CommandCompleteFlag) {
            SDHC_ClearInterruptStatusFlags(SD_SDHC, kSDHC_CommandCompleteFlag);
            return true;
        }
        if (flags & (kSDHC_CommandTimeoutFlag | kSDHC_CommandCrcErrorFlag |
                     kSDHC_CommandEndBitErrorFlag | kSDHC_CommandIndexErrorFlag)) {
            SDHC_ClearInterruptStatusFlags(SD_SDHC,
                kSDHC_CommandTimeoutFlag | kSDHC_CommandCrcErrorFlag |
                kSDHC_CommandEndBitErrorFlag | kSDHC_CommandIndexErrorFlag);
            return false;
        }
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if ((now - start) >= timeout_ms) return false;
        vTaskDelay(1U);
    }
}

/* -----------------------------------------------------------------------
 * 私有：等待 SDHC 数据传输完成
 * ----------------------------------------------------------------------- */
static bool sdhc_wait_data_done(uint32_t timeout_ms)
{
    uint32_t start = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    while (true) {
        uint32_t flags = SDHC_GetInterruptStatusFlags(SD_SDHC);
        if (flags & kSDHC_DataCompleteFlag) {
            SDHC_ClearInterruptStatusFlags(SD_SDHC, kSDHC_DataCompleteFlag);
            return true;
        }
        if (flags & (kSDHC_DataTimeoutFlag | kSDHC_DataCrcErrorFlag |
                     kSDHC_DataEndBitErrorFlag)) {
            SDHC_ClearInterruptStatusFlags(SD_SDHC,
                kSDHC_DataTimeoutFlag | kSDHC_DataCrcErrorFlag |
                kSDHC_DataEndBitErrorFlag);
            return false;
        }
        uint32_t now = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if ((now - start) >= timeout_ms) return false;
        vTaskDelay(1U);
    }
}

/* -----------------------------------------------------------------------
 * 私有：发送 SD 命令（无数据）
 * ----------------------------------------------------------------------- */
static bool sd_send_cmd(uint32_t cmd_idx, uint32_t arg,
                        uint32_t resp_type, uint32_t *resp)
{
    sdhc_command_t cmd = {
        .index        = cmd_idx,
        .argument     = arg,
        .responseType = (sdhc_response_type_t)resp_type,
    };
    sdhc_transfer_t xfer = {
        .command = &cmd,
        .data    = NULL,
    };

    status_t ret = SDHC_TransferBlocking(SD_SDHC, NULL, &xfer);
    if (ret != kStatus_Success) return false;

    if (resp != NULL) {
        resp[0] = cmd.response[0];
        resp[1] = cmd.response[1];
        resp[2] = cmd.response[2];
        resp[3] = cmd.response[3];
    }
    return true;
}

/* -----------------------------------------------------------------------
 * 私有：SD 卡初始化序列（CMD0 -> CMD8 -> ACMD41 -> CMD2 -> CMD3 -> CMD7）
 * ----------------------------------------------------------------------- */
static bool sd_card_init(void)
{
    uint32_t resp[4];

    /* CMD0: GO_IDLE_STATE */
    sd_send_cmd(0U, 0U, kSDHC_ResponseTypeNone, NULL);
    vTaskDelay(pdMS_TO_TICKS(10U));

    /* CMD8: SEND_IF_COND（检测 SD 2.0+） */
    bool is_v2 = sd_send_cmd(8U, 0x000001AAU, kSDHC_ResponseTypeR7, resp);
    if (is_v2 && (resp[0] & 0xFFU) != 0xAAU) {
        is_v2 = false;  /* 电压不匹配 */
    }

    /* ACMD41: SD_SEND_OP_COND（轮询直到卡就绪） */
    uint32_t ocr_arg = is_v2 ? 0x40FF8000U : 0x00FF8000U;
    uint32_t retry   = 0U;
    bool     ready   = false;

    while (retry++ < 100U) {
        /* CMD55: APP_CMD */
        sd_send_cmd(55U, 0U, kSDHC_ResponseTypeR1, NULL);
        /* ACMD41 */
        if (sd_send_cmd(41U, ocr_arg, kSDHC_ResponseTypeR3, resp)) {
            if (resp[0] & 0x80000000U) {  /* 卡就绪位 */
                s_sd.high_capacity = (resp[0] & 0x40000000U) ? true : false;
                ready = true;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10U));
    }

    if (!ready) return false;

    /* CMD2: ALL_SEND_CID */
    if (!sd_send_cmd(2U, 0U, kSDHC_ResponseTypeR2, resp)) return false;

    /* CMD3: SEND_RELATIVE_ADDR */
    if (!sd_send_cmd(3U, 0U, kSDHC_ResponseTypeR6, resp)) return false;
    s_sd.rca = resp[0] & 0xFFFF0000U;

    /* CMD9: SEND_CSD（获取卡容量） */
    if (sd_send_cmd(9U, s_sd.rca, kSDHC_ResponseTypeR2, resp)) {
        if (s_sd.high_capacity) {
            /* SDHC/SDXC: C_SIZE 在 CSD[69:48] */
            uint32_t c_size = ((resp[1] & 0x3FFFFFU) << 8U) |
                              ((resp[2] >> 24U) & 0xFFU);
            s_sd.sector_count = (c_size + 1U) * 1024U;
        } else {
            /* SDSC: 旧版计算方式 */
            uint32_t read_bl_len = (resp[1] >> 16U) & 0xFU;
            uint32_t c_size      = ((resp[1] & 0x3FFU) << 2U) |
                                   ((resp[2] >> 30U) & 0x3U);
            uint32_t c_size_mult = (resp[2] >> 15U) & 0x7U;
            s_sd.sector_count    = (c_size + 1U) *
                                   (1U << (c_size_mult + 2U)) *
                                   (1U << read_bl_len) / SD_SECTOR_SIZE;
        }
    }

    /* CMD7: SELECT_CARD */
    if (!sd_send_cmd(7U, s_sd.rca, kSDHC_ResponseTypeR1b, resp)) return false;

    /* ACMD6: SET_BUS_WIDTH = 4 位 */
    sd_send_cmd(55U, s_sd.rca, kSDHC_ResponseTypeR1, NULL);
    sd_send_cmd(6U,  2U,       kSDHC_ResponseTypeR1, NULL);
    SDHC_SetDataBusWidth(SD_SDHC, kSDHC_DataBusWidth4Bit);

    /* CMD16: SET_BLOCKLEN = 512 */
    sd_send_cmd(16U, SD_SECTOR_SIZE, kSDHC_ResponseTypeR1, NULL);

    /* 提升时钟到 25MHz（高速模式） */
    SDHC_SetSdClock(SD_SDHC, CLOCK_GetFreq(kCLOCK_CoreSysClk), 25000000U);

    return true;
}

/* -----------------------------------------------------------------------
 * disk_initialize
 * ----------------------------------------------------------------------- */
DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0U) return STA_NOINIT;  /* 只支持卷 0 */

    /* 检测 SD 卡插入 */
    if (GPIO_PinRead(SD_CD_GPIO, SD_CD_PIN) != 0U) {
        s_sd.card_present = false;
        return STA_NODISK;
    }
    s_sd.card_present = true;

    /* 初始化 SDHC 控制器（400kHz 识别时钟） */
    sdhc_config_t sdhc_cfg = {
        .cardDetectDat3      = false,
        .endianMode          = kSDHC_EndianModeLittle,
        .dmaMode             = kSDHC_DmaModeAdma2,
        .readWatermarkLevel  = 128U,
        .writeWatermarkLevel = 128U,
    };
    SDHC_Init(SD_SDHC, &sdhc_cfg);
    SDHC_SetSdClock(SD_SDHC, CLOCK_GetFreq(kCLOCK_CoreSysClk), 400000U);

    /* 发送 74+ 个时钟脉冲初始化卡 */
    SDHC_SetCardActive(SD_SDHC, 100U);
    vTaskDelay(pdMS_TO_TICKS(10U));

    /* 执行 SD 卡初始化序列 */
    if (!sd_card_init()) {
        s_sd.initialized = false;
        return STA_NOINIT;
    }

    s_sd.initialized = true;
    return 0U;  /* 初始化成功 */
}

/* -----------------------------------------------------------------------
 * disk_status
 * ----------------------------------------------------------------------- */
DSTATUS disk_status(BYTE pdrv)
{
    if (pdrv != 0U) return STA_NOINIT;

    if (GPIO_PinRead(SD_CD_GPIO, SD_CD_PIN) != 0U) {
        s_sd.initialized  = false;
        s_sd.card_present = false;
        return STA_NODISK;
    }
    if (!s_sd.initialized) return STA_NOINIT;
    return 0U;
}

/* -----------------------------------------------------------------------
 * disk_read
 * ----------------------------------------------------------------------- */
DRESULT disk_read(BYTE pdrv, BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0U || !s_sd.initialized) return RES_NOTRDY;
    if (buff == NULL || count == 0U)     return RES_PARERR;

    /* 构造读命令（CMD17 单块 / CMD18 多块） */
    uint32_t addr = s_sd.high_capacity ? sector : (sector * SD_SECTOR_SIZE);
    uint32_t cmd_idx = (count == 1U) ? 17U : 18U;

    sdhc_data_t data = {
        .enableAutoCommand12 = (count > 1U),
        .enableIgnoreError   = false,
        .dataType            = kSDHC_TransferDataNormal,
        .blockSize           = SD_SECTOR_SIZE,
        .blockCount          = count,
        .rxData              = (uint32_t *)(void *)buff,
        .txData              = NULL,
    };

    sdhc_command_t cmd = {
        .index        = cmd_idx,
        .argument     = addr,
        .responseType = kSDHC_ResponseTypeR1,
    };

    sdhc_transfer_t xfer = {
        .command = &cmd,
        .data    = &data,
    };

    status_t ret = SDHC_TransferBlocking(SD_SDHC, s_adma2_table, &xfer);
    if (ret != kStatus_Success) {
        return RES_ERROR;
    }

    return RES_OK;
}

/* -----------------------------------------------------------------------
 * disk_write
 * ----------------------------------------------------------------------- */
DRESULT disk_write(BYTE pdrv, const BYTE *buff, LBA_t sector, UINT count)
{
    if (pdrv != 0U || !s_sd.initialized) return RES_NOTRDY;
    if (buff == NULL || count == 0U)     return RES_PARERR;

    uint32_t addr    = s_sd.high_capacity ? sector : (sector * SD_SECTOR_SIZE);
    uint32_t cmd_idx = (count == 1U) ? 24U : 25U;

    sdhc_data_t data = {
        .enableAutoCommand12 = (count > 1U),
        .enableIgnoreError   = false,
        .dataType            = kSDHC_TransferDataNormal,
        .blockSize           = SD_SECTOR_SIZE,
        .blockCount          = count,
        .rxData              = NULL,
        .txData              = (const uint32_t *)(const void *)buff,
    };

    sdhc_command_t cmd = {
        .index        = cmd_idx,
        .argument     = addr,
        .responseType = kSDHC_ResponseTypeR1,
    };

    sdhc_transfer_t xfer = {
        .command = &cmd,
        .data    = &data,
    };

    status_t ret = SDHC_TransferBlocking(SD_SDHC, s_adma2_table, &xfer);
    if (ret != kStatus_Success) {
        return RES_ERROR;
    }

    return RES_OK;
}

/* -----------------------------------------------------------------------
 * disk_ioctl
 * ----------------------------------------------------------------------- */
DRESULT disk_ioctl(BYTE pdrv, BYTE cmd, void *buff)
{
    if (pdrv != 0U || !s_sd.initialized) return RES_NOTRDY;

    switch (cmd) {
    case CTRL_SYNC:
        /* SDHC 写操作是同步的，无需额外操作 */
        return RES_OK;

    case GET_SECTOR_COUNT:
        if (buff == NULL) return RES_PARERR;
        *(LBA_t *)buff = (LBA_t)s_sd.sector_count;
        return RES_OK;

    case GET_SECTOR_SIZE:
        if (buff == NULL) return RES_PARERR;
        *(WORD *)buff = (WORD)SD_SECTOR_SIZE;
        return RES_OK;

    case GET_BLOCK_SIZE:
        /* SD 卡擦除块大小（扇区数），返回 1 表示按扇区擦除 */
        if (buff == NULL) return RES_PARERR;
        *(DWORD *)buff = 1U;
        return RES_OK;

    case CTRL_TRIM:
        /* 不支持 TRIM，直接返回 OK */
        return RES_OK;

    default:
        return RES_PARERR;
    }
}

/* -----------------------------------------------------------------------
 * get_fattime
 * 返回当前时间（FatFs 文件时间戳）
 * 格式：bit[31:25]=年-1980, [24:21]=月, [20:16]=日,
 *        [15:11]=时, [10:5]=分, [4:0]=秒/2
 * 优先使用 RTC 真实时间，RTC 不可用时返回固定时间
 * ----------------------------------------------------------------------- */
DWORD get_fattime(void)
{
    rtc_datetime_t dt;
    if (rtc_is_valid() && rtc_get_datetime(&dt) == IPCAM_OK) {
        return ((DWORD)(dt.year  - 1980U) << 25U) |
               ((DWORD)dt.month           << 21U) |
               ((DWORD)dt.day             << 16U) |
               ((DWORD)dt.hour            << 11U) |
               ((DWORD)dt.minute          <<  5U) |
               ((DWORD)(dt.second / 2U));
    }
    /* RTC 不可用：返回固定时间 2026-01-01 00:00:00 */
    return ((DWORD)(2026U - 1980U) << 25U) |
           ((DWORD)1U  << 21U) |
           ((DWORD)1U  << 16U) |
           ((DWORD)0U  << 11U) |
           ((DWORD)0U  <<  5U) |
           ((DWORD)0U);
}
