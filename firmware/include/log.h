#ifndef LOG_H
#define LOG_H

/**
 * @file log.h
 * @brief 串口日志模块接口
 *
 * 支持带时间戳和级别的格式化日志输出，使用环形缓冲区异步发送。
 */

#include <stdint.h>
#include "ipcam_types.h"

/* -----------------------------------------------------------------------
 * 日志级别
 * ----------------------------------------------------------------------- */
typedef enum {
    LOG_LEVEL_DEBUG = 0,
    LOG_LEVEL_INFO,
    LOG_LEVEL_WARN,
    LOG_LEVEL_ERROR,
} log_level_t;

/* 编译期最低日志级别（低于此级别的日志被编译器优化掉） */
#ifndef LOG_MIN_LEVEL
#define LOG_MIN_LEVEL LOG_LEVEL_DEBUG
#endif

/* -----------------------------------------------------------------------
 * 日志宏（带模块标签）
 * ----------------------------------------------------------------------- */
#define LOG_D(tag, fmt, ...) log_write(LOG_LEVEL_DEBUG, tag, fmt, ##__VA_ARGS__)
#define LOG_I(tag, fmt, ...) log_write(LOG_LEVEL_INFO,  tag, fmt, ##__VA_ARGS__)
#define LOG_W(tag, fmt, ...) log_write(LOG_LEVEL_WARN,  tag, fmt, ##__VA_ARGS__)
#define LOG_E(tag, fmt, ...) log_write(LOG_LEVEL_ERROR, tag, fmt, ##__VA_ARGS__)

/* -----------------------------------------------------------------------
 * 函数声明
 * ----------------------------------------------------------------------- */

/**
 * @brief 初始化日志模块（配置 UART，启动 DMA 发送）
 * @return IPCAM_OK 或错误码
 */
ipcam_status_t log_init(void);

/**
 * @brief 写入一条日志（线程安全，可在 ISR 中调用）
 * @param level  日志级别
 * @param tag    模块标签字符串（如 "CAM", "NET"）
 * @param fmt    printf 格式字符串
 */
void log_write(log_level_t level, const char *tag, const char *fmt, ...);

/**
 * @brief 将日志缓冲区中的内容立即刷新到串口（阻塞）
 *        用于系统复位前确保日志不丢失。
 */
void log_flush(void);

/**
 * @brief 获取当前系统时间戳（ms），用于日志时间戳
 */
uint32_t log_get_timestamp_ms(void);

#endif /* LOG_H */
