/**
 * @file rtc_driver.c
 * @brief RTC 驱动实现
 *
 * 优先级：DS3231（外部，精度高）> K64 内置 RTC > tick 估算
 *
 * DS3231 寄存器映射（BCD 编码）：
 *   0x00 = 秒    0x01 = 分    0x02 = 时
 *   0x03 = 星期  0x04 = 日    0x05 = 月/世纪
 *   0x06 = 年（00–99，相对 2000 年）
 */

#include "rtc_driver.h"
#include "log.h"
#include "board.h"
#include "fsl_rtc.h"
#include "fsl_i2c.h"
#include "fsl_port.h"
#include "fsl_clock.h"
#include "FreeRTOS.h"
#include "task.h"
#include <string.h>
#include <stdio.h>

#define TAG "RTC"

/* -----------------------------------------------------------------------
 * DS3231 寄存器地址
 * ----------------------------------------------------------------------- */
#define DS3231_REG_SECONDS   0x00U
#define DS3231_REG_MINUTES   0x01U
#define DS3231_REG_HOURS     0x02U
#define DS3231_REG_DAY       0x03U
#define DS3231_REG_DATE      0x04U
#define DS3231_REG_MONTH     0x05U
#define DS3231_REG_YEAR      0x06U
#define DS3231_REG_CONTROL   0x0EU
#define DS3231_REG_STATUS    0x0FU

/* DS3231 控制寄存器默认值：禁用 SQW 输出，启用振荡器 */
#define DS3231_CTRL_DEFAULT  0x1CU

/* -----------------------------------------------------------------------
 * BCD 转换宏
 * ----------------------------------------------------------------------- */
#define BCD_TO_DEC(bcd)  ((uint8_t)(((bcd) >> 4U) * 10U + ((bcd) & 0x0FU)))
#define DEC_TO_BCD(dec)  ((uint8_t)((((dec) / 10U) << 4U) | ((dec) % 10U)))

/* -----------------------------------------------------------------------
 * 驱动状态
 * ----------------------------------------------------------------------- */
static rtc_source_t s_rtc_source = RTC_SOURCE_NONE;
static bool         s_initialized = false;

/* 前向声明（rtc_init 中需要调用，定义在文件末尾） */
static bool rtc_is_valid_dt_internal(const rtc_datetime_t *dt);

/* -----------------------------------------------------------------------
 * DS3231 I2C 操作
 * ----------------------------------------------------------------------- */
static ipcam_status_t ds3231_write_reg(uint8_t reg, uint8_t val)
{
    uint8_t buf[2] = {reg, val};
    i2c_master_transfer_t xfer = {
        .slaveAddress   = RTC_DS3231_ADDR,
        .direction      = kI2C_Write,
        .subaddress     = 0U,
        .subaddressSize = 0U,
        .data           = buf,
        .dataSize       = 2U,
        .flags          = kI2C_TransferDefaultFlag,
    };
    return (I2C_MasterTransferBlocking(RTC_DS3231_I2C, &xfer) == kStatus_Success)
           ? IPCAM_OK : IPCAM_ERR_IO;
}

static ipcam_status_t ds3231_read_regs(uint8_t start_reg,
                                        uint8_t *buf, uint8_t len)
{
    /* 先写寄存器地址 */
    i2c_master_transfer_t xfer = {
        .slaveAddress   = RTC_DS3231_ADDR,
        .direction      = kI2C_Write,
        .subaddress     = 0U,
        .subaddressSize = 0U,
        .data           = &start_reg,
        .dataSize       = 1U,
        .flags          = kI2C_TransferNoStopFlag,  /* 不发 STOP，紧接 Repeated START */
    };
    if (I2C_MasterTransferBlocking(RTC_DS3231_I2C, &xfer) != kStatus_Success) {
        return IPCAM_ERR_IO;
    }

    /* 再读数据 */
    xfer.direction = kI2C_Read;
    xfer.data      = buf;
    xfer.dataSize  = len;
    xfer.flags     = kI2C_TransferRepeatedStartFlag;
    return (I2C_MasterTransferBlocking(RTC_DS3231_I2C, &xfer) == kStatus_Success)
           ? IPCAM_OK : IPCAM_ERR_IO;
}

/* -----------------------------------------------------------------------
 * DS3231 存在性检测
 * ----------------------------------------------------------------------- */
static bool ds3231_detect(void)
{
    uint8_t dummy = 0U;
    i2c_master_transfer_t xfer = {
        .slaveAddress   = RTC_DS3231_ADDR,
        .direction      = kI2C_Read,
        .subaddress     = 0U,
        .subaddressSize = 0U,
        .data           = &dummy,
        .dataSize       = 1U,
        .flags          = kI2C_TransferDefaultFlag,
    };
    return (I2C_MasterTransferBlocking(RTC_DS3231_I2C, &xfer) == kStatus_Success);
}

/* -----------------------------------------------------------------------
 * DS3231 读取时间
 * ----------------------------------------------------------------------- */
static ipcam_status_t ds3231_read_datetime(rtc_datetime_t *dt)
{
    uint8_t regs[7];
    ipcam_status_t ret = ds3231_read_regs(DS3231_REG_SECONDS, regs, 7U);
    if (ret != IPCAM_OK) return ret;

    dt->second = BCD_TO_DEC(regs[0] & 0x7FU);
    dt->minute = BCD_TO_DEC(regs[1] & 0x7FU);
    dt->hour   = BCD_TO_DEC(regs[2] & 0x3FU);  /* 24h 模式 */
    /* regs[3] = 星期，跳过 */
    dt->day    = BCD_TO_DEC(regs[4] & 0x3FU);
    dt->month  = BCD_TO_DEC(regs[5] & 0x1FU);
    dt->year   = 2000U + BCD_TO_DEC(regs[6]);

    /* 世纪位（月寄存器 bit7） */
    if (regs[5] & 0x80U) {
        dt->year += 100U;
    }

    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * DS3231 写入时间
 * ----------------------------------------------------------------------- */
static ipcam_status_t ds3231_write_datetime(const rtc_datetime_t *dt)
{
    /* 清除振荡器停止标志（OSF） */
    ds3231_write_reg(DS3231_REG_STATUS, 0x00U);

    uint8_t year_bcd  = DEC_TO_BCD((uint8_t)((dt->year - 2000U) % 100U));
    uint8_t month_bcd = DEC_TO_BCD(dt->month);
    /* 若年份 >= 2100，设置世纪位 */
    if (dt->year >= 2100U) {
        month_bcd |= 0x80U;
    }

    /* 批量写入 7 个寄存器（从 0x00 开始） */
    uint8_t buf[8] = {
        DS3231_REG_SECONDS,
        DEC_TO_BCD(dt->second),
        DEC_TO_BCD(dt->minute),
        DEC_TO_BCD(dt->hour),   /* 24h 模式，bit6=0 */
        0x01U,                   /* 星期（固定为 1，不使用） */
        DEC_TO_BCD(dt->day),
        month_bcd,
        year_bcd,
    };

    i2c_master_transfer_t xfer = {
        .slaveAddress   = RTC_DS3231_ADDR,
        .direction      = kI2C_Write,
        .subaddress     = 0U,
        .subaddressSize = 0U,
        .data           = buf,
        .dataSize       = sizeof(buf),
        .flags          = kI2C_TransferDefaultFlag,
    };
    return (I2C_MasterTransferBlocking(RTC_DS3231_I2C, &xfer) == kStatus_Success)
           ? IPCAM_OK : IPCAM_ERR_IO;
}

/* -----------------------------------------------------------------------
 * K64 内置 RTC 操作
 * ----------------------------------------------------------------------- */
static ipcam_status_t k64_rtc_init_hw(void)
{
    rtc_config_t rtc_cfg;
    RTC_GetDefaultConfig(&rtc_cfg);
    rtc_cfg.compensationInterval = 0U;
    rtc_cfg.compensationTime     = 0U;
    rtc_cfg.wakeupSelect         = false;
    rtc_cfg.updateMode           = false;
    rtc_cfg.supervisorAccess     = false;
    RTC_Init(RTC, &rtc_cfg);

    /* 若 RTC 振荡器未运行，启动它 */
    if (!(RTC->SR & RTC_SR_TIF_MASK)) {
        /* 时间有效，振荡器已在运行 */
        return IPCAM_OK;
    }

    /* 时间无效（首次上电或 VBAT 耗尽），设置初始时间 2026-01-01 00:00:00 */
    RTC_StopTimer(RTC);
    rtc_datetime_t default_dt = {
        .year = 2026U, .month = 1U, .day = 1U,
        .hour = 0U, .minute = 0U, .second = 0U,
    };
    rtc_datetime_t sdk_dt = {
        .year   = default_dt.year,
        .month  = default_dt.month,
        .day    = default_dt.day,
        .hour   = default_dt.hour,
        .minute = default_dt.minute,
        .second = default_dt.second,
    };
    /* KSDK RTC 使用相同的字段名，直接赋值 */
    RTC_SetDatetime(RTC, (rtc_datetime_t *)&sdk_dt);
    RTC_StartTimer(RTC);

    LOG_W(TAG, "K64 RTC time was invalid, set to 2026-01-01 00:00:00");
    return IPCAM_OK;
}

static ipcam_status_t k64_rtc_read(rtc_datetime_t *dt)
{
    rtc_datetime_t sdk_dt;
    RTC_GetDatetime(RTC, &sdk_dt);
    dt->year   = sdk_dt.year;
    dt->month  = (uint8_t)sdk_dt.month;
    dt->day    = (uint8_t)sdk_dt.day;
    dt->hour   = (uint8_t)sdk_dt.hour;
    dt->minute = (uint8_t)sdk_dt.minute;
    dt->second = (uint8_t)sdk_dt.second;
    return IPCAM_OK;
}

static ipcam_status_t k64_rtc_write(const rtc_datetime_t *dt)
{
    RTC_StopTimer(RTC);
    rtc_datetime_t sdk_dt = {
        .year   = dt->year,
        .month  = dt->month,
        .day    = dt->day,
        .hour   = dt->hour,
        .minute = dt->minute,
        .second = dt->second,
    };
    status_t ret = RTC_SetDatetime(RTC, &sdk_dt);
    RTC_StartTimer(RTC);
    return (ret == kStatus_Success) ? IPCAM_OK : IPCAM_ERR_INVALID;
}

/* -----------------------------------------------------------------------
 * DS3231 I2C1 初始化
 * ----------------------------------------------------------------------- */
static void ds3231_i2c_init(void)
{
    CLOCK_EnableClock(kCLOCK_PortC);
    port_pin_config_t i2c_cfg = {
        .pullSelect          = kPORT_PullUp,
        .slewRate            = kPORT_FastSlewRate,
        .passiveFilterEnable = kPORT_PassiveFilterDisable,
        .openDrainEnable     = kPORT_OpenDrainEnable,
        .driveStrength       = kPORT_HighDriveStrength,
        .mux                 = RTC_DS3231_I2C_MUX,
        .lockRegister        = kPORT_UnlockRegister,
    };
    PORT_SetPinConfig(RTC_DS3231_I2C_SCL_PORT, RTC_DS3231_I2C_SCL_PIN, &i2c_cfg);
    PORT_SetPinConfig(RTC_DS3231_I2C_SDA_PORT, RTC_DS3231_I2C_SDA_PIN, &i2c_cfg);

    i2c_master_config_t master_cfg;
    I2C_MasterGetDefaultConfig(&master_cfg);
    master_cfg.baudRate_Bps = RTC_DS3231_I2C_BAUDRATE;
    I2C_MasterInit(RTC_DS3231_I2C, &master_cfg, CLOCK_GetFreq(kCLOCK_BusClk));
}

/* -----------------------------------------------------------------------
 * rtc_init
 * ----------------------------------------------------------------------- */
ipcam_status_t rtc_init(void)
{
    /* 1. 初始化 K64 内置 RTC（始终初始化，作为后备） */
    ipcam_status_t ret = k64_rtc_init_hw();
    if (ret != IPCAM_OK) {
        LOG_E(TAG, "K64 internal RTC init failed: %d", (int)ret);
        return ret;
    }
    s_rtc_source = RTC_SOURCE_INTERNAL;

    /* 2. 尝试初始化 DS3231 */
    ds3231_i2c_init();
    vTaskDelay(pdMS_TO_TICKS(10U));

    if (ds3231_detect()) {
        LOG_I(TAG, "DS3231 detected on I2C1");

        /* 配置 DS3231 控制寄存器 */
        ds3231_write_reg(DS3231_REG_CONTROL, DS3231_CTRL_DEFAULT);

        /* 检查 DS3231 振荡器停止标志（OSF） */
        uint8_t status_reg = 0U;
        ds3231_read_regs(DS3231_REG_STATUS, &status_reg, 1U);
        bool ds3231_valid = !(status_reg & 0x80U);  /* OSF=0 表示时间有效 */

        if (ds3231_valid) {
            /* 从 DS3231 读取时间，同步到 K64 内置 RTC */
            rtc_datetime_t ds_dt;
            if (ds3231_read_datetime(&ds_dt) == IPCAM_OK && rtc_is_valid_dt_internal(&ds_dt)) {
                k64_rtc_write(&ds_dt);
                s_rtc_source = RTC_SOURCE_DS3231;
                LOG_I(TAG, "Time synced from DS3231: %04u-%02u-%02u %02u:%02u:%02u UTC",
                      ds_dt.year, ds_dt.month, ds_dt.day,
                      ds_dt.hour, ds_dt.minute, ds_dt.second);
            } else {
                LOG_W(TAG, "DS3231 time invalid, using internal RTC");
            }
        } else {
            LOG_W(TAG, "DS3231 OSF set (power loss detected), time unreliable");
            /* 将内置 RTC 时间写入 DS3231，清除 OSF */
            rtc_datetime_t internal_dt;
            k64_rtc_read(&internal_dt);
            ds3231_write_datetime(&internal_dt);
            s_rtc_source = RTC_SOURCE_DS3231;
        }
    } else {
        LOG_I(TAG, "DS3231 not found, using K64 internal RTC only");
    }

    s_initialized = true;

    rtc_datetime_t now;
    rtc_get_datetime(&now);
    LOG_I(TAG, "RTC initialized [source=%s]: %04u-%02u-%02u %02u:%02u:%02u UTC",
          (s_rtc_source == RTC_SOURCE_DS3231) ? "DS3231" : "Internal",
          now.year, now.month, now.day,
          now.hour, now.minute, now.second);

    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * rtc_get_datetime
 * ----------------------------------------------------------------------- */
ipcam_status_t rtc_get_datetime(rtc_datetime_t *dt)
{
    if (dt == NULL) return IPCAM_ERR_INVALID;
    if (!s_initialized) return IPCAM_ERR_NOT_INIT;

    /* 始终从 K64 内置 RTC 读取（已在 init 时与 DS3231 同步） */
    return k64_rtc_read(dt);
}

/* -----------------------------------------------------------------------
 * rtc_set_datetime
 * ----------------------------------------------------------------------- */
ipcam_status_t rtc_set_datetime(const rtc_datetime_t *dt)
{
    if (dt == NULL) return IPCAM_ERR_INVALID;

    /* 写入 K64 内置 RTC */
    ipcam_status_t ret = k64_rtc_write(dt);
    if (ret != IPCAM_OK) return ret;

    /* 若 DS3231 存在，同步写入 */
    if (s_rtc_source == RTC_SOURCE_DS3231) {
        ret = ds3231_write_datetime(dt);
        if (ret != IPCAM_OK) {
            LOG_W(TAG, "DS3231 write failed: %d", (int)ret);
        }
    }

    LOG_I(TAG, "RTC set to %04u-%02u-%02u %02u:%02u:%02u UTC",
          dt->year, dt->month, dt->day,
          dt->hour, dt->minute, dt->second);
    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * rtc_get_unix_timestamp
 * 简化版 mktime（不处理闰秒，精度到秒）
 * ----------------------------------------------------------------------- */
uint32_t rtc_get_unix_timestamp(void)
{
    if (!s_initialized) return 0U;

    rtc_datetime_t dt;
    if (rtc_get_datetime(&dt) != IPCAM_OK) return 0U;

    /* 使用 K64 内置 RTC 的秒计数器（从 1970-01-01 起） */
    /* KSDK RTC_GetDatetime 内部已维护 Unix 时间戳 */
    uint32_t unix_ts = 0U;
    RTC_GetDatetime(RTC, (rtc_datetime_t *)&dt);  /* 重新读取确保一致 */

    /* 手动计算 Unix 时间戳 */
    /* 每月天数（非闰年） */
    static const uint16_t days_in_month[12] = {
        31U, 28U, 31U, 30U, 31U, 30U, 31U, 31U, 30U, 31U, 30U, 31U
    };

    uint32_t year  = dt.year;
    uint32_t month = dt.month;
    uint32_t day   = dt.day;

    /* 从 1970 年起累计天数 */
    uint32_t days = 0U;
    for (uint32_t y = 1970U; y < year; y++) {
        bool leap = ((y % 4U == 0U) && (y % 100U != 0U)) || (y % 400U == 0U);
        days += leap ? 366U : 365U;
    }
    for (uint32_t m = 1U; m < month; m++) {
        days += days_in_month[m - 1U];
        /* 2 月闰年补 1 天 */
        if (m == 2U) {
            bool leap = ((year % 4U == 0U) && (year % 100U != 0U)) ||
                        (year % 400U == 0U);
            if (leap) days++;
        }
    }
    days += day - 1U;

    unix_ts = days * 86400U
            + (uint32_t)dt.hour   * 3600U
            + (uint32_t)dt.minute * 60U
            + (uint32_t)dt.second;

    return unix_ts;
}

/* -----------------------------------------------------------------------
 * rtc_format_filename  →  YYYYMMDD_HHMMSS
 * ----------------------------------------------------------------------- */
void rtc_format_filename(const rtc_datetime_t *dt, char *buf)
{
    if (dt == NULL || buf == NULL) return;
    snprintf(buf, 16U, "%04u%02u%02u_%02u%02u%02u",
             dt->year, dt->month, dt->day,
             dt->hour, dt->minute, dt->second);
}

/* -----------------------------------------------------------------------
 * rtc_format_iso8601  →  YYYY-MM-DDTHH:MM:SSZ
 * ----------------------------------------------------------------------- */
void rtc_format_iso8601(const rtc_datetime_t *dt, char *buf)
{
    if (dt == NULL || buf == NULL) return;
    snprintf(buf, 21U, "%04u-%02u-%02uT%02u:%02u:%02uZ",
             dt->year, dt->month, dt->day,
             dt->hour, dt->minute, dt->second);
}

/* -----------------------------------------------------------------------
 * rtc_get_source
 * ----------------------------------------------------------------------- */
rtc_source_t rtc_get_source(void)
{
    return s_rtc_source;
}

/* -----------------------------------------------------------------------
 * rtc_is_valid_dt_internal（静态内部实现，供 rtc_init 前向调用）
 * rtc_is_valid_dt（公开接口，声明在头文件）
 * rtc_is_valid
 * ----------------------------------------------------------------------- */
static bool rtc_is_valid_dt_internal(const rtc_datetime_t *dt)
{
    return (dt != NULL && dt->year >= 2024U && dt->year <= 2099U
            && dt->month >= 1U && dt->month <= 12U
            && dt->day >= 1U && dt->day <= 31U);
}

bool rtc_is_valid_dt(const rtc_datetime_t *dt)
{
    return rtc_is_valid_dt_internal(dt);
}

bool rtc_is_valid(void)
{
    if (!s_initialized) return false;
    rtc_datetime_t dt;
    if (rtc_get_datetime(&dt) != IPCAM_OK) return false;
    return rtc_is_valid_dt_internal(&dt);
}
