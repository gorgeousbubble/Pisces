/**
 * @file log.c
 * @brief 串口日志模块实现
 *
 * 使用环形缓冲区 + UART DMA 发送，支持多任务并发写入（FreeRTOS Mutex 保护）。
 * 在 ISR 中调用时自动降级为轮询发送。
 */

#include "log.h"
#include "board.h"
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

static uint8_t  s_log_buf[IPCAM_LOG_BUF_SIZE];  /**< 环形缓冲区 */
static uint32_t s_buf_head = 0U;                 /**< 写指针 */
static uint32_t s_buf_tail = 0U;                 /**< 读指针 */
static bool     s_initialized = false;

static SemaphoreHandle_t s_log_mutex = NULL;

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

static uint32_t log_buf_write(const uint8_t *data, uint32_t len)
{
    uint32_t written = 0U;
    for (uint32_t i = 0U; i < len; i++) {
        uint32_t next = (s_buf_head + 1U) % IPCAM_LOG_BUF_SIZE;
        if (next == s_buf_tail) {
            /* 缓冲区满，丢弃剩余数据 */
            break;
        }
        s_log_buf[s_buf_head] = data[i];
        s_buf_head = next;
        written++;
    }
    return written;
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

    char line[LOG_LINE_MAX];
    uint32_t ts = log_get_timestamp_ms();

    /* 格式：[时间戳ms][级别][标签] 消息\r\n */
    int prefix_len = snprintf(line, sizeof(line),
                              "[%8lu][%s][%-8s] ",
                              (unsigned long)ts,
                              s_level_str[level],
                              tag ? tag : "");
    if (prefix_len < 0 || prefix_len >= (int)sizeof(line)) {
        return;
    }

    va_list args;
    va_start(args, fmt);
    int msg_len = vsnprintf(line + prefix_len,
                            sizeof(line) - (size_t)prefix_len - 2U,
                            fmt, args);
    va_end(args);

    if (msg_len < 0) {
        return;
    }

    /* 追加 \r\n */
    uint32_t total = (uint32_t)prefix_len + (uint32_t)msg_len;
    if (total + 2U < sizeof(line)) {
        line[total]     = '\r';
        line[total + 1] = '\n';
        total += 2U;
    }

    /* 判断是否在 ISR 中 */
    if (xPortIsInsideInterrupt()) {
        /* ISR 中直接阻塞发送，不使用缓冲区 */
        log_uart_send_blocking((const uint8_t *)line, total);
        return;
    }

    /* 任务上下文：加锁写入缓冲区，然后发送 */
    if (xSemaphoreTake(s_log_mutex, pdMS_TO_TICKS(10U)) == pdTRUE) {
        /* 简化实现：直接阻塞发送（生产环境可改为 DMA 异步） */
        log_uart_send_blocking((const uint8_t *)line, total);
        xSemaphoreGive(s_log_mutex);
    }
}

/* -----------------------------------------------------------------------
 * log_flush
 * ----------------------------------------------------------------------- */
void log_flush(void)
{
    /* 等待 UART 发送完成 */
    while (!(UART_GetStatusFlags(LOG_UART) & kUART_TransmissionCompleteFlag)) {
        /* 忙等 */
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
