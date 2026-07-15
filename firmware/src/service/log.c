/**
 * @file log.c
 * @brief 串口日志模块实现
 *
 * 通过 UART0 阻塞发送，多任务并发写入由 FreeRTOS Mutex 保护。
 * 在 ISR 中调用时使用独立栈缓冲，不加锁直接发送。
 */

#include "log.h"
#include "board.h"
#include "ipcam_config.h"
#include "fsl_uart.h"
#include "fsl_clock.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <stdio.h>
#include <string.h>
#include <stdarg.h>

/* -----------------------------------------------------------------------
 * 私有数据
 * ----------------------------------------------------------------------- */
#define LOG_LINE_MAX   256U   /**< 单行日志最大字节数 */

static bool     s_initialized = false;
static SemaphoreHandle_t s_log_mutex = NULL;

/* 任务上下文共享的行缓冲区（由 s_log_mutex 保护，避免占用每个任务的栈） */
static char     s_line[LOG_LINE_MAX];

static const char * const s_level_str[] = {
    "D", "I", "W", "E"
};

/* -----------------------------------------------------------------------
 * 私有函数
 * ----------------------------------------------------------------------- */
static void log_uart_send_blocking(const uint8_t *data, uint32_t len)
{
    UART_WriteBlocking(LOG_UART, data, len);
}

/* 将日志格式化到指定缓冲区，返回总长度（含 \r\n），0 表示失败 */
static uint32_t log_format(char *buf, log_level_t level,
                           const char *tag, const char *fmt, va_list args)
{
    uint32_t ts = log_get_timestamp_ms();

    int prefix_len = snprintf(buf, LOG_LINE_MAX,
                              "[%8lu][%s][%-8s] ",
                              (unsigned long)ts,
                              s_level_str[level],
                              tag ? tag : "");
    if (prefix_len < 0 || prefix_len >= (int)LOG_LINE_MAX) {
        return 0U;
    }

    int msg_len = vsnprintf(buf + prefix_len,
                            LOG_LINE_MAX - (size_t)prefix_len - 2U,
                            fmt, args);
    if (msg_len < 0) {
        return 0U;
    }

    uint32_t total = (uint32_t)prefix_len + (uint32_t)msg_len;
    if (total + 2U < LOG_LINE_MAX) {
        buf[total]     = '\r';
        buf[total + 1] = '\n';
        total += 2U;
    }
    return total;
}

/* -----------------------------------------------------------------------
 * log_init
 * ----------------------------------------------------------------------- */
ipcam_status_t log_init(void)
{
    uart_config_t config;
    UART_GetDefaultConfig(&config);
    config.baudRate_Bps = LOG_UART_BAUDRATE;
    config.enableTx     = true;
    config.enableRx     = true;

    uint32_t clk = CLOCK_GetFreq(LOG_UART_CLKSRC);
    status_t ret = UART_Init(LOG_UART, &config, clk);
    if (ret != kStatus_Success) {
        return IPCAM_ERR_HW;
    }

    s_log_mutex = xSemaphoreCreateMutex();
    if (s_log_mutex == NULL) {
        return IPCAM_ERR_NOMEM;
    }

    s_initialized = true;

    /* 打印启动横幅 */
    const char *banner =
        "\r\n"
        "========================================\r\n"
        "  K64 IP Camera Firmware v1.0.0\r\n"
        "  NXP MK64FN1M0VLL12 @ 120MHz\r\n"
        "========================================\r\n";
    log_uart_send_blocking((const uint8_t *)banner, strlen(banner));

    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * log_write
 * ----------------------------------------------------------------------- */
void log_write(log_level_t level, const char *tag, const char *fmt, ...)
{
    if (!s_initialized || level < LOG_MIN_LEVEL) {
        return;
    }

    va_list args;

    /* ISR 上下文：用独立栈缓冲，不加锁直接发送 */
    if (__get_IPSR() != 0U) {
        char isr_line[LOG_LINE_MAX];
        va_start(args, fmt);
        uint32_t total = log_format(isr_line, level, tag, fmt, args);
        va_end(args);
        if (total > 0U) {
            log_uart_send_blocking((const uint8_t *)isr_line, total);
        }
        return;
    }

    /* 任务上下文：加锁后使用共享 static 缓冲，格式化和发送均在锁内 */
    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(10U)) == pdTRUE) {
        va_start(args, fmt);
        uint32_t total = log_format(s_line, level, tag, fmt, args);
        va_end(args);
        if (total > 0U) {
            log_uart_send_blocking((const uint8_t *)s_line, total);
        }
        xSemaphoreGive(s_log_mutex);
    }
}

/* -----------------------------------------------------------------------
 * log_flush
 * ----------------------------------------------------------------------- */
void log_flush(void)
{
    /* 有界忙等：等待 UART 发送完成，但设自旋上限。
     *
     * log_flush 主要用于 sys_soft_reset() 复位前刷出日志，属于错误恢复路径。
     * 若 UART 外设异常（TC 标志永不置位），无上限忙等会导致复位永远无法执行，
     * 使本应触发复位的故障演变为彻底挂死。故设纯软件自旋上限（不依赖
     * tick/中断，兼容 HardFault/栈溢出钩子等异常上下文），超限即放弃刷新。
     *
     * 约 200 万次空循环，@120MHz 上限约数十毫秒，足够正常发送完成。 */
    uint32_t guard = 0U;
    while (!(UART_GetStatusFlags(LOG_UART) & kUART_TransmissionCompleteFlag)) {
        if (++guard >= 2000000U) {
            break;
        }
    }
}

/* -----------------------------------------------------------------------
 * log_get_timestamp_ms
 * ----------------------------------------------------------------------- */
uint32_t log_get_timestamp_ms(void)
{
    if (xTaskGetSchedulerState() != taskSCHEDULER_NOT_STARTED) {
        return (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    }
    /* 调度器未启动时使用 SysTick 估算 */
    return 0U;
}
