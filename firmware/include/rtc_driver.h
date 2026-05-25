#ifndef RTC_DRIVER_H
#define RTC_DRIVER_H

/**
 * @file rtc_driver.h
 * @brief RTC 驱动接口
 *
 * 支持两种 RTC 源，优先级：
 *   1. 外部 DS3231（I2C1，精度 ±2ppm，掉电保持）
 *   2. K64 内置 RTC（VBAT 供电，精度较低）
 *
 * 上电时自动检测 DS3231 是否存在：
 *   - 存在：从 DS3231 读取时间，同步到 K64 内置 RTC
 *   - 不存在：仅使用 K64 内置 RTC
 *
 * 所有时间以 UTC 表示，调用方负责时区转换（如需要）。
 */

#include "ipcam_types.h"
#include <stdint.h>
#include <stdbool.h>

/* 包含 KSDK RTC 驱动头文件，复用其 rtc_datetime_t 定义，避免重复定义冲突 */
#include "fsl_rtc.h"

/* -----------------------------------------------------------------------
 * 时间结构体
 *
 * rtc_datetime_t 直接使用 fsl_rtc.h 中的定义（KSDK 2.x）：
 *   typedef struct { uint16_t year; uint8_t month; uint8_t day;
 *                    uint8_t hour;  uint8_t minute; uint8_t second; }
 *                    rtc_datetime_t;
 * 两者字段完全一致，无需重复定义。
 * ----------------------------------------------------------------------- */

/* -----------------------------------------------------------------------
 * RTC 源枚举
 * ----------------------------------------------------------------------- */
typedef enum {
    RTC_SOURCE_NONE     = 0,  /**< 未初始化 */
    RTC_SOURCE_INTERNAL = 1,  /**< K64 内置 RTC */
    RTC_SOURCE_DS3231   = 2,  /**< 外部 DS3231 */
} rtc_source_t;

/* -----------------------------------------------------------------------
 * DS3231 硬件配置（I2C1，与摄像头 I2C0 分开）
 * ----------------------------------------------------------------------- */
#define RTC_DS3231_I2C          I2C1
#define RTC_DS3231_I2C_SCL_PORT PORTC
#define RTC_DS3231_I2C_SCL_PIN  10U   /**< PTC10 - I2C1_SCL */
#define RTC_DS3231_I2C_SDA_PORT PORTC
#define RTC_DS3231_I2C_SDA_PIN  11U   /**< PTC11 - I2C1_SDA */
#define RTC_DS3231_I2C_MUX      kPORT_MuxAlt2
#define RTC_DS3231_I2C_BAUDRATE 400000U
#define RTC_DS3231_ADDR         0x68U /**< DS3231 固定 I2C 地址 */

/* -----------------------------------------------------------------------
 * 函数声明
 * ----------------------------------------------------------------------- */

/**
 * @brief 初始化 RTC 驱动
 *
 * 检测 DS3231 → 若存在则同步到内置 RTC → 启动内置 RTC 计数。
 * 若两者均不可用，则以系统 tick 作为时间基准（精度低）。
 *
 * @return IPCAM_OK 或错误码
 */
ipcam_status_t rtc_init(void);

/**
 * @brief 获取当前 UTC 时间
 * @param[out] dt  输出时间结构体
 * @return IPCAM_OK / IPCAM_ERR_NOT_INIT
 */
ipcam_status_t rtc_get_datetime(rtc_datetime_t *dt);

/**
 * @brief 设置当前 UTC 时间（同时写入 DS3231 和内置 RTC）
 * @param dt  要设置的时间
 * @return IPCAM_OK / IPCAM_ERR_IO
 */
ipcam_status_t rtc_set_datetime(const rtc_datetime_t *dt);

/**
 * @brief 获取当前 Unix 时间戳（UTC 秒，从 1970-01-01 00:00:00 起）
 * @return Unix 时间戳，0 表示 RTC 未初始化
 */
uint32_t rtc_get_unix_timestamp(void);

/**
 * @brief 将时间格式化为文件名字符串 YYYYMMDD_HHMMSS
 * @param dt   时间结构体
 * @param buf  输出缓冲区（至少 16 字节）
 */
void rtc_format_filename(const rtc_datetime_t *dt, char *buf);

/**
 * @brief 将时间格式化为 ISO 8601 字符串 YYYY-MM-DDTHH:MM:SSZ
 * @param dt   时间结构体
 * @param buf  输出缓冲区（至少 21 字节）
 */
void rtc_format_iso8601(const rtc_datetime_t *dt, char *buf);

/**
 * @brief 获取当前活跃的 RTC 源
 */
rtc_source_t rtc_get_source(void);

/**
 * @brief 检查 RTC 时间是否有效（年份 >= 2024 视为有效）
 */
bool rtc_is_valid(void);

/**
 * @brief 检查时间结构体是否有效（内部辅助，也可外部调用）
 */
bool rtc_is_valid_dt(const rtc_datetime_t *dt);

#endif /* RTC_DRIVER_H */
