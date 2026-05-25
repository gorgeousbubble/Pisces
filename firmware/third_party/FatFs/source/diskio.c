/**
 * @file diskio.c
 * @brief FatFs 底层磁盘 I/O — SDK_2_11_0_FRDM-K64F 适配
 *
 * SDK 2.11.0 fsl_sdhc.h 的 SDHC_TransferBlocking 签名：
 *   status_t SDHC_TransferBlocking(SDHC_Type *base,
 *                                   sdhc_adma2_descriptor_t *adma2Table,
 *                                   uint32_t adma2TableWords,
 *                                   sdhc_transfer_t *transfer);
 *
 * sdhc_data_t 字段（SDK 2.11.0）：
 *   bool     enableAutoCommand12
 *   bool     enableAutoCommand23
 *   bool     enableIgnoreError
 *   uint32_t blockSize
 *   uint32_t blockCount
 *   uint32_t *rxData
 *   const uint32_t *txData
 *   注意：没有 dataType 字段（该字段在更新版本中才有）
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
 * 常量
 * ----------------------------------------------------------------------- */
#define SD_SECTOR_SIZE      512U
#define ADMA2_TABLE_WORDS   8U    /* 描述符表大小（words） */

/* -----------------------------------------------------------------------
 * SD 卡状态
 * ----------------------------------------------------------------------- */
typedef struct {
    bool     initialized;
    bool     card_present;
    uint32_t sector_count;
    uint32_t rca;
    bool     high_capacity;
} sd_state_t;

static sd_state_t s_sd;

/* ADMA2 描述符表，4 字节对齐 */
static sdhc_adma2_descriptor_t s_adma2_table[ADMA2_TABLE_WORDS]
    __attribute__((aligned(4)));

/* -----------------------------------------------------------------------
 * 私有：发送 SD 命令
 * ----------------------------------------------------------------------- */
static bool sd_send_cmd(uint32_t cmd_idx, uint32_t arg,
                        sdhc_response_type_t resp_type, uint32_t *resp)
{
    sdhc_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.index        = cmd_idx;
    cmd.argument     = arg;
    cmd.responseType = resp_type;

    sdhc_transfer_t xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.command = &cmd;
    xfer.data    = NULL;

    if (SDHC_TransferBlocking(SD_SDHC, s_adma2_table, ADMA2_TABLE_WORDS, &xfer)
            != kStatus_Success) {
        return false;
    }

    if (resp != NULL) {
        resp[0] = cmd.response[0];
        resp[1] = cmd.response[1];
        resp[2] = cmd.response[2];
        resp[3] = cmd.response[3];
    }
    return true;
}

/* -----------------------------------------------------------------------
 * 私有：SD 卡初始化序列
 * ----------------------------------------------------------------------- */
static bool sd_card_init(void)
{
    uint32_t resp[4];

    sd_send_cmd(0U, 0U, kSDHC_ResponseTypeNone, NULL);
    vTaskDelay(pdMS_TO_TICKS(10U));

    bool is_v2 = sd_send_cmd(8U, 0x000001AAU, kSDHC_ResponseTypeR7, resp);
    if (is_v2 && (resp[0] & 0xFFU) != 0xAAU) {
        is_v2 = false;
    }

    uint32_t ocr_arg = is_v2 ? 0x40FF8000U : 0x00FF8000U;
    uint32_t retry   = 0U;
    bool     ready   = false;

    while (retry++ < 100U) {
        sd_send_cmd(55U, 0U, kSDHC_ResponseTypeR1, NULL);
        if (sd_send_cmd(41U, ocr_arg, kSDHC_ResponseTypeR3, resp)) {
            if (resp[0] & 0x80000000U) {
                s_sd.high_capacity = (resp[0] & 0x40000000U) ? true : false;
                ready = true;
                break;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10U));
    }
    if (!ready) return false;

    if (!sd_send_cmd(2U, 0U, kSDHC_ResponseTypeR2, resp)) return false;
    if (!sd_send_cmd(3U, 0U, kSDHC_ResponseTypeR6, resp)) return false;
    s_sd.rca = resp[0] & 0xFFFF0000U;

    if (sd_send_cmd(9U, s_sd.rca, kSDHC_ResponseTypeR2, resp)) {
        if (s_sd.high_capacity) {
            uint32_t c_size = ((resp[1] & 0x3FFFFFU) << 8U) |
                              ((resp[2] >> 24U) & 0xFFU);
            s_sd.sector_count = (c_size + 1U) * 1024U;
        } else {
            uint32_t read_bl_len = (resp[1] >> 16U) & 0xFU;
            uint32_t c_size      = ((resp[1] & 0x3FFU) << 2U) |
                                   ((resp[2] >> 30U) & 0x3U);
            uint32_t c_size_mult = (resp[2] >> 15U) & 0x7U;
            s_sd.sector_count    = (c_size + 1U) *
                                   (1U << (c_size_mult + 2U)) *
                                   (1U << read_bl_len) / SD_SECTOR_SIZE;
        }
    }

    if (!sd_send_cmd(7U, s_sd.rca, kSDHC_ResponseTypeR1b, resp)) return false;

    sd_send_cmd(55U, s_sd.rca, kSDHC_ResponseTypeR1, NULL);
    sd_send_cmd(6U,  2U,       kSDHC_ResponseTypeR1, NULL);
    SDHC_SetDataBusWidth(SD_SDHC, kSDHC_DataBusWidth4Bit);

    sd_send_cmd(16U, SD_SECTOR_SIZE, kSDHC_ResponseTypeR1, NULL);

    SDHC_SetSdClock(SD_SDHC, CLOCK_GetFreq(kCLOCK_CoreSysClk), 25000000U);

    return true;
}

/* -----------------------------------------------------------------------
 * disk_initialize
 * ----------------------------------------------------------------------- */
DSTATUS disk_initialize(BYTE pdrv)
{
    if (pdrv != 0U) return STA_NOINIT;

    if (GPIO_PinRead(SD_CD_GPIO, SD_CD_PIN) != 0U) {
        s_sd.card_present = false;
        return STA_NODISK;
    }
    s_sd.card_present = true;

    sdhc_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.cardDetectDat3      = false;
    cfg.endianMode          = kSDHC_EndianModeLittle;
    cfg.dmaMode             = kSDHC_DmaModeAdma2;
    cfg.readWatermarkLevel  = 128U;
    cfg.writeWatermarkLevel = 128U;

    SDHC_Init(SD_SDHC, &cfg);
    SDHC_SetSdClock(SD_SDHC, CLOCK_GetFreq(kCLOCK_CoreSysClk), 400000U);
    SDHC_SetCardActive(SD_SDHC, 100U);
    vTaskDelay(pdMS_TO_TICKS(10U));

    if (!sd_card_init()) {
        s_sd.initialized = false;
        return STA_NOINIT;
    }

    s_sd.initialized = true;
    return 0U;
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

    uint32_t addr = s_sd.high_capacity ? (uint32_t)sector
                                       : (uint32_t)sector * SD_SECTOR_SIZE;

    sdhc_data_t data;
    memset(&data, 0, sizeof(data));
    data.enableAutoCommand12 = (count > 1U);
    data.enableIgnoreError   = false;
    data.blockSize           = SD_SECTOR_SIZE;
    data.blockCount          = (uint32_t)count;
    data.rxData              = (uint32_t *)(void *)buff;
    data.txData              = NULL;

    sdhc_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.index        = (count == 1U) ? 17U : 18U;
    cmd.argument     = addr;
    cmd.responseType = kSDHC_ResponseTypeR1;

    sdhc_transfer_t xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.command = &cmd;
    xfer.data    = &data;

    if (SDHC_TransferBlocking(SD_SDHC, s_adma2_table, ADMA2_TABLE_WORDS, &xfer)
            != kStatus_Success) {
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

    uint32_t addr = s_sd.high_capacity ? (uint32_t)sector
                                       : (uint32_t)sector * SD_SECTOR_SIZE;

    sdhc_data_t data;
    memset(&data, 0, sizeof(data));
    data.enableAutoCommand12 = (count > 1U);
    data.enableIgnoreError   = false;
    data.blockSize           = SD_SECTOR_SIZE;
    data.blockCount          = (uint32_t)count;
    data.rxData              = NULL;
    data.txData              = (const uint32_t *)(const void *)buff;

    sdhc_command_t cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.index        = (count == 1U) ? 24U : 25U;
    cmd.argument     = addr;
    cmd.responseType = kSDHC_ResponseTypeR1;

    sdhc_transfer_t xfer;
    memset(&xfer, 0, sizeof(xfer));
    xfer.command = &cmd;
    xfer.data    = &data;

    if (SDHC_TransferBlocking(SD_SDHC, s_adma2_table, ADMA2_TABLE_WORDS, &xfer)
            != kStatus_Success) {
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
        if (buff == NULL) return RES_PARERR;
        *(DWORD *)buff = 1U;
        return RES_OK;
    case CTRL_TRIM:
        return RES_OK;
    default:
        return RES_PARERR;
    }
}

/* -----------------------------------------------------------------------
 * get_fattime
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
    return ((DWORD)(2026U - 1980U) << 25U) |
           ((DWORD)1U << 21U) |
           ((DWORD)1U << 16U);
}
