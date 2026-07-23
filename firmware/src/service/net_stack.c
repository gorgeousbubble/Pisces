/**
 * @file net_stack.c
 * @brief WiFi 网络传输层实现（ESP8266/ESP32 AT 指令驱动）
 *
 * 通信协议：
 *   - UART1 AT 指令控制 ESP8266/ESP32
 *   - WiFi 连接：AT+CWJAP
 *   - TCP 连接：AT+CIPSTART（单路 TCP 客户端）
 *   - 视频流推送：AT+CIPSEND + MJPEG-over-HTTP multipart
 *   - 命令接收：复用同一 TCP 连接的反向通道（服务器主动推送 JSON）
 *
 * 状态机：
 *   IDLE -> WIFI_CONNECTING -> WIFI_CONNECTED
 *        -> TCP_CONNECTING  -> STREAMING
 *        -> OFFLINE（重试耗尽）
 */

#include "net_stack.h"
#include "net_auth.h"
#include "log.h"
#include "board.h"
#include "sys_manager.h"
#include "ipcam_config.h"
#include "fsl_uart.h"
#include "fsl_gpio.h"
#include "fsl_clock.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TAG "NET"

/* -----------------------------------------------------------------------
 * AT 指令超时常量
 * ----------------------------------------------------------------------- */
#define AT_RESP_TIMEOUT_MS       3000U   /**< 普通 AT 命令响应超时 */
#define AT_WIFI_TIMEOUT_MS       30000U  /**< WiFi 连接超时 */
#define AT_TCP_TIMEOUT_MS        10000U  /**< TCP 连接超时 */
#define AT_SEND_TIMEOUT_MS       2000U   /**< 单次 CIPSEND 超时 */
#define AT_PROMPT_TIMEOUT_MS     1000U   /**< 等待 '>' 提示符超时 */

/* -----------------------------------------------------------------------
 * UART 接收环形缓冲区
 * ----------------------------------------------------------------------- */
#define UART_RX_BUF_SIZE   1024U

typedef struct {
    uint8_t  buf[UART_RX_BUF_SIZE];
    uint32_t head;   /**< 写指针（ISR 写） */
    uint32_t tail;   /**< 读指针（任务读） */
} uart_ring_buf_t;

static volatile uart_ring_buf_t s_rx_buf;

/* -----------------------------------------------------------------------
 * 活跃网络任务心跳 ID
 *
 * net_connect / net_send_* 等函数可能同步阻塞数秒到数分钟（WiFi/TCP 重试），
 * 远超 HEARTBEAT_TIMEOUT_MS(3s)。若阻塞期间不刷新调用任务的心跳，
 * sys_manager 会误判该任务已死并触发软复位——断网即重启死循环。
 *
 * 因此在所有长等待点周期性调用 sys_heartbeat_update(s_active_hb_id)，
 * 由调用任务通过 net_set_active_heartbeat() 指明自己的心跳 ID。
 * 默认 HEARTBEAT_NET_SEND（绝大多数网络阻塞发生在推流任务）。
 * ----------------------------------------------------------------------- */
static volatile heartbeat_id_t s_active_hb_id = HEARTBEAT_NET_SEND;

/* 长等待期间喂心跳的节流间隔（ms） */
#define CONN_HEARTBEAT_FEED_MS   500U

void net_set_active_heartbeat(heartbeat_id_t id)
{
    if (id < HEARTBEAT_COUNT) {
        s_active_hb_id = id;
    }
}

/* 分段延时，期间周期性刷新活跃任务心跳，避免长睡眠触发心跳超时复位 */
static void conn_delay_with_heartbeat(uint32_t total_ms)
{
    uint32_t elapsed = 0U;
    while (elapsed < total_ms) {
        uint32_t step = (total_ms - elapsed) < CONN_HEARTBEAT_FEED_MS
                        ? (total_ms - elapsed) : CONN_HEARTBEAT_FEED_MS;
        sys_heartbeat_update(s_active_hb_id);
        vTaskDelay(pdMS_TO_TICKS(step));
        elapsed += step;
    }
    sys_heartbeat_update(s_active_hb_id);
}

/* -----------------------------------------------------------------------
 * 命令接收缓冲区（存储服务器下发的 JSON 命令）
 *
 * 写者：task_net_send（在 at_wait_response 中检测到 JSON 行时）
 * 读者：task_cmd_handler（通过 net_recv_cmd 轮询）
 * 两者是不同任务，必须通过互斥锁保护，否则可能出现：
 *   - strncpy 写入过程中被读者抢占，读到一半新一半旧的“撕裂”数据
 *   - s_cmd_pending 标志与 s_cmd_buf 内容不一致
 * ----------------------------------------------------------------------- */
static char     s_cmd_buf[IPCAM_CMD_BUF_SIZE];
static bool     s_cmd_pending = false;
static SemaphoreHandle_t s_cmd_mutex = NULL;

/* JSON 命令字段键（用 sizeof(key)-1 计算跳过长度，避免硬编码偏移量） */
#define JSON_KEY_CMD      "\"cmd\":"
#define JSON_KEY_QUALITY  "\"quality\":"
#define JSON_KEY_FPS      "\"fps\":"
#define JSON_KEY_WIDTH    "\"width\":"
#define JSON_KEY_HEIGHT   "\"height\":"

/* -----------------------------------------------------------------------
 * 网络层状态
 * ----------------------------------------------------------------------- */
typedef struct {
    bool             initialized;
    net_state_t      state;
    ipcam_config_t   cfg;
    uint32_t         wifi_retry_count;
    uint32_t         tcp_retry_count;
    uint32_t         last_connected_ms;   /**< 上次 WiFi 连接成功时间戳 */
    uint32_t         disconnect_start_ms; /**< WiFi 断开开始时间戳（用于 60s 硬复位） */
    bool             http_header_sent;    /**< HTTP multipart 头是否已发送 */
    SemaphoreHandle_t tx_mutex;           /**< 发送互斥锁 */
} net_state_data_t;

static net_state_data_t s_net;

/* 状态上报请求标志：由其他任务置位，task_net_send 在空闲时消费，
 * 避免 net_send_status 从多个任务调用时与帧发送竞争 tx_mutex */
static volatile bool s_status_report_pending = false;

void net_request_status_report(void)
{
    s_status_report_pending = true;
}

bool net_status_report_pending(void)
{
    return s_status_report_pending;
}

void net_clear_status_report(void)
{
    s_status_report_pending = false;
}

/* -----------------------------------------------------------------------
 * UART4 接收中断处理（将数据写入环形缓冲区）
 * 修复：原为 UART1_RX_TX_IRQHandler，已改用 UART4 避免 PTC3/PTC4 冲突
 * ----------------------------------------------------------------------- */
void UART4_RX_TX_IRQHandler(void)
{
    uint32_t flags = UART_GetStatusFlags(WIFI_UART);

    if (flags & kUART_RxDataRegFullFlag) {
        uint8_t byte = (uint8_t)UART_ReadByte(WIFI_UART);
        uint32_t next = (s_rx_buf.head + 1U) % UART_RX_BUF_SIZE;
        if (next != s_rx_buf.tail) {
            s_rx_buf.buf[s_rx_buf.head] = byte;
            s_rx_buf.head = next;
        }
        /* 缓冲区满时丢弃最新字节（保护旧数据） */
    }

    if (flags & kUART_RxOverrunFlag) {
        UART_ClearStatusFlags(WIFI_UART, kUART_RxOverrunFlag);
    }
}

/* -----------------------------------------------------------------------
 * 私有：从环形缓冲区读取一个字节（非阻塞）
 * ----------------------------------------------------------------------- */
static bool uart_rx_read_byte(uint8_t *byte)
{
    if (s_rx_buf.tail == s_rx_buf.head) {
        return false;  /* 缓冲区空 */
    }
    *byte = s_rx_buf.buf[s_rx_buf.tail];
    s_rx_buf.tail = (s_rx_buf.tail + 1U) % UART_RX_BUF_SIZE;
    return true;
}

/* -----------------------------------------------------------------------
 * 私有：清空接收缓冲区
 * ----------------------------------------------------------------------- */
static void uart_rx_flush(void)
{
    s_rx_buf.tail = s_rx_buf.head;
}

/* -----------------------------------------------------------------------
 * 私有：发送 AT 命令字符串（阻塞）
 * ----------------------------------------------------------------------- */
static void at_send(const char *cmd)
{
    UART_WriteBlocking(WIFI_UART, (const uint8_t *)cmd, strlen(cmd));
}

/* -----------------------------------------------------------------------
 * 私有：等待响应中包含指定字符串，超时返回 false
 * resp_buf 和 resp_len 可为 NULL（不需要保存响应内容时）
 * ----------------------------------------------------------------------- */
static bool at_wait_response(const char *expected,
                             uint32_t    timeout_ms,
                             char       *resp_buf,
                             uint32_t    resp_buf_len)
{
    char    line[256];
    uint32_t line_pos = 0U;
    uint32_t start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t last_hb_ms = start_ms;

    while (true) {
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if ((now_ms - start_ms) >= timeout_ms) {
            return false;  /* 超时 */
        }

        /* 长等待期间周期性喂心跳，防止 sys_manager 误判任务死亡 */
        if ((now_ms - last_hb_ms) >= CONN_HEARTBEAT_FEED_MS) {
            sys_heartbeat_update(s_active_hb_id);
            last_hb_ms = now_ms;
        }

        uint8_t byte;
        if (!uart_rx_read_byte(&byte)) {
            vTaskDelay(1U);  /* 让出 CPU 1ms */
            continue;
        }

        /* 积累到行缓冲区 */
        if (byte == '\n') {
            line[line_pos] = '\0';
            /* 去除行尾 \r */
            if (line_pos > 0U && line[line_pos - 1U] == '\r') {
                line[line_pos - 1U] = '\0';
            }

            /* 保存响应内容 */
            if (resp_buf != NULL && resp_buf_len > 0U) {
                uint32_t copy_len = (uint32_t)strlen(line);
                if (copy_len >= resp_buf_len) copy_len = resp_buf_len - 1U;
                strncpy(resp_buf, line, copy_len);
                resp_buf[copy_len] = '\0';
            }

            /* 检查是否包含期望字符串 */
            if (strstr(line, expected) != NULL) {
                return true;
            }

            /* 检查是否为错误响应 */
            if (strstr(line, "ERROR") != NULL ||
                strstr(line, "FAIL")  != NULL) {
                LOG_W(TAG, "AT error response: %s", line);
                return false;
            }

            /* 检查是否为服务器下发的命令 JSON。
             * 从行内定位 '{' 起始，兼容两种情形：
             *   - 裸 JSON 行： {"cmd":...}
             *   - ESP 普通模式带前缀： +IPD,<len>:{"cmd":...}
             * 并要求包含 "cmd" 字段，避免把其它 JSON/AT 响应误判为命令。 */
            {
                char *json_start = strchr(line, '{');
                if (json_start != NULL && strstr(json_start, "\"cmd\"") != NULL) {
                    if (s_cmd_mutex != NULL &&
                        xSemaphoreTake(s_cmd_mutex, pdMS_TO_TICKS(50U)) == pdTRUE) {
                        strncpy(s_cmd_buf, json_start, IPCAM_CMD_BUF_SIZE - 1U);
                        s_cmd_buf[IPCAM_CMD_BUF_SIZE - 1U] = '\0';
                        s_cmd_pending = true;
                        xSemaphoreGive(s_cmd_mutex);
                        LOG_D(TAG, "Command received: %s", s_cmd_buf);
                    } else {
                        LOG_W(TAG, "Command buffer busy, command dropped");
                    }
                }
            }

            line_pos = 0U;
        } else if (byte != '\r') {
            if (line_pos < sizeof(line) - 1U) {
                line[line_pos++] = (char)byte;
            }
        }
    }
}

/* -----------------------------------------------------------------------
 * 私有：等待 '>' 提示符（CIPSEND 数据发送提示）
 * ----------------------------------------------------------------------- */
static bool at_wait_prompt(uint32_t timeout_ms)
{
    uint32_t start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t last_hb_ms = start_ms;
    while (true) {
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        if ((now_ms - start_ms) >= timeout_ms) return false;

        if ((now_ms - last_hb_ms) >= CONN_HEARTBEAT_FEED_MS) {
            sys_heartbeat_update(s_active_hb_id);
            last_hb_ms = now_ms;
        }

        uint8_t byte;
        if (uart_rx_read_byte(&byte) && byte == '>') {
            return true;
        }
        vTaskDelay(1U);
    }
}

/* -----------------------------------------------------------------------
 * 私有：通过 AT+CIPSEND 发送二进制数据
 * ----------------------------------------------------------------------- */
static ipcam_status_t at_cipsend(const uint8_t *data, uint32_t size)
{
    char cmd[32];
    snprintf(cmd, sizeof(cmd), "AT+CIPSEND=%lu\r\n", (unsigned long)size);
    uart_rx_flush();
    at_send(cmd);

    if (!at_wait_prompt(AT_PROMPT_TIMEOUT_MS)) {
        LOG_W(TAG, "CIPSEND: no '>' prompt");
        return IPCAM_ERR_TIMEOUT;
    }

    /* 发送数据 */
    UART_WriteBlocking(WIFI_UART, data, size);

    /* 等待 SEND OK */
    if (!at_wait_response("SEND OK", AT_SEND_TIMEOUT_MS, NULL, 0U)) {
        LOG_W(TAG, "CIPSEND: no SEND OK");
        return IPCAM_ERR_IO;
    }
    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * net_reinit_at — 重新发送 AT 基础初始化（硬件复位后调用）
 * ----------------------------------------------------------------------- */
void net_reinit_at(void)
{
    uart_rx_flush();
    at_send("ATE0\r\n");
    at_wait_response("OK", AT_RESP_TIMEOUT_MS, NULL, 0U);
    at_send("AT+CWMODE=1\r\n");
    at_wait_response("OK", AT_RESP_TIMEOUT_MS, NULL, 0U);
    s_net.state              = NET_STATE_IDLE;
    s_net.wifi_retry_count   = 0U;
    s_net.tcp_retry_count    = 0U;
    LOG_I(TAG, "AT re-initialized after module reset");
}

/* -----------------------------------------------------------------------
 * net_init
 * ----------------------------------------------------------------------- */
ipcam_status_t net_init(const ipcam_config_t *cfg)
{
    if (cfg == NULL) return IPCAM_ERR_INVALID;

    memset(&s_net, 0, sizeof(s_net));
    s_net.cfg   = *cfg;
    s_net.state = NET_STATE_IDLE;

    /* 初始化 UART1 */
    uart_config_t uart_cfg;
    UART_GetDefaultConfig(&uart_cfg);
    uart_cfg.baudRate_Bps = WIFI_UART_BAUDRATE;
    uart_cfg.enableTx     = true;
    uart_cfg.enableRx     = true;

    uint32_t clk = CLOCK_GetFreq(WIFI_UART_CLKSRC);
    status_t ret = UART_Init(WIFI_UART, &uart_cfg, clk);
    if (ret != kStatus_Success) {
        LOG_E(TAG, "UART1 init failed");
        return IPCAM_ERR_HW;
    }

    /* 使能接收中断 */
    UART_EnableInterrupts(WIFI_UART, kUART_RxDataRegFullInterruptEnable |
                                     kUART_RxOverrunInterruptEnable);
    NVIC_SetPriority(WIFI_UART_IRQ, configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY + 1U);
    EnableIRQ(WIFI_UART_IRQ);

    /* 创建发送互斥锁 */
    s_net.tx_mutex = xSemaphoreCreateMutex();
    if (s_net.tx_mutex == NULL) {
        return IPCAM_ERR_NOMEM;
    }

    /* 创建命令缓冲区互斥锁（保护 s_cmd_buf / s_cmd_pending 跨任务访问） */
    s_cmd_mutex = xSemaphoreCreateMutex();
    if (s_cmd_mutex == NULL) {
        return IPCAM_ERR_NOMEM;
    }

    /* 硬件复位 WiFi 模块，确保初始状态干净 */
    net_reset_wifi_module();
    vTaskDelay(pdMS_TO_TICKS(500U));

    /* 基础 AT 测试 */
    uart_rx_flush();
    at_send("AT\r\n");
    if (!at_wait_response("OK", AT_RESP_TIMEOUT_MS, NULL, 0U)) {
        LOG_W(TAG, "WiFi module not responding to AT");
        /* 不返回错误，允许后续重试 */
    }

    /* 关闭回显 */
    at_send("ATE0\r\n");
    at_wait_response("OK", AT_RESP_TIMEOUT_MS, NULL, 0U);

    /* 设置为 Station 模式 */
    at_send("AT+CWMODE=1\r\n");
    at_wait_response("OK", AT_RESP_TIMEOUT_MS, NULL, 0U);

    s_net.initialized = true;
    LOG_I(TAG, "Network layer initialized");
    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * 私有：连接 WiFi
 * ----------------------------------------------------------------------- */
static ipcam_status_t wifi_connect(void)
{
    s_net.state = NET_STATE_WIFI_CONNECTING;
    LOG_I(TAG, "Connecting to WiFi: %s", s_net.cfg.wifi_ssid);

    char cmd[128];
    snprintf(cmd, sizeof(cmd), "AT+CWJAP=\"%s\",\"%s\"\r\n",
             s_net.cfg.wifi_ssid, s_net.cfg.wifi_password);

    uart_rx_flush();
    at_send(cmd);

    if (!at_wait_response("WIFI CONNECTED", AT_WIFI_TIMEOUT_MS, NULL, 0U)) {
        LOG_W(TAG, "WiFi connect failed");
        s_net.state = NET_STATE_IDLE;
        return IPCAM_ERR_TIMEOUT;
    }

    /* 等待获取 IP */
    at_wait_response("WIFI GOT IP", 5000U, NULL, 0U);

    s_net.state              = NET_STATE_WIFI_CONNECTED;
    s_net.last_connected_ms  = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    s_net.wifi_retry_count   = 0U;
    LOG_I(TAG, "WiFi connected");
    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * 私有：建立 TCP 连接并发送 HTTP 推流头
 * ----------------------------------------------------------------------- */
static ipcam_status_t tcp_connect_and_start_stream(void)
{
    s_net.state = NET_STATE_TCP_CONNECTING;
    LOG_I(TAG, "Connecting TCP to %s:%u",
          s_net.cfg.server_ip, s_net.cfg.server_port);

    char cmd[80];
    snprintf(cmd, sizeof(cmd), "AT+CIPSTART=\"TCP\",\"%s\",%u\r\n",
             s_net.cfg.server_ip, s_net.cfg.server_port);

    uart_rx_flush();
    at_send(cmd);

    if (!at_wait_response("CONNECT", AT_TCP_TIMEOUT_MS, NULL, 0U)) {
        LOG_W(TAG, "TCP connect failed");
        s_net.state = NET_STATE_WIFI_CONNECTED;
        return IPCAM_ERR_TIMEOUT;
    }

    /* 发送 HTTP POST 推流头（带 HMAC 认证） */
    char http_header[384];
    uint32_t ts_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    char hmac_hex[HMAC_HEX_LEN];
    net_auth_sign("POST", "/stream", ts_ms, NULL, 0U, hmac_hex);

    int hlen = snprintf(http_header, sizeof(http_header),
        "POST /stream HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: multipart/x-mixed-replace; boundary=frame\r\n"
        "Transfer-Encoding: chunked\r\n"
        "Connection: keep-alive\r\n"
        "X-Timestamp: %lu\r\n"
        "X-HMAC-SHA256: %s\r\n"
        "\r\n",
        s_net.cfg.server_ip, s_net.cfg.server_port,
        (unsigned long)ts_ms, hmac_hex);

    if (hlen > 0) {
        ipcam_status_t ret = at_cipsend((const uint8_t *)http_header, (uint32_t)hlen);
        if (ret != IPCAM_OK) {
            LOG_W(TAG, "Failed to send HTTP header");
            s_net.state = NET_STATE_WIFI_CONNECTED;
            return ret;
        }
    }

    s_net.state              = NET_STATE_STREAMING;
    s_net.tcp_retry_count    = 0U;
    s_net.http_header_sent   = true;
    LOG_I(TAG, "TCP connected, streaming started");
    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * net_connect
 * ----------------------------------------------------------------------- */
ipcam_status_t net_connect(void)
{
    if (!s_net.initialized) return IPCAM_ERR_NOT_INIT;

    /* WiFi 连接（带重试） */
    for (uint32_t i = 0U; i < IPCAM_WIFI_RETRY_MAX; i++) {
        if (wifi_connect() == IPCAM_OK) break;
        if (i < IPCAM_WIFI_RETRY_MAX - 1U) {
            LOG_W(TAG, "WiFi retry %lu/%u in %ums",
                  (unsigned long)(i + 1U), IPCAM_WIFI_RETRY_MAX,
                  IPCAM_WIFI_RETRY_INTERVAL_MS);
            conn_delay_with_heartbeat(IPCAM_WIFI_RETRY_INTERVAL_MS);
        }
    }

    if (s_net.state != NET_STATE_WIFI_CONNECTED) {
        LOG_E(TAG, "WiFi connect failed after %u retries, going offline",
              IPCAM_WIFI_RETRY_MAX);
        s_net.state = NET_STATE_OFFLINE;
        s_net.disconnect_start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
        return IPCAM_ERR_TIMEOUT;
    }

    /* TCP 连接（带重试） */
    for (uint32_t i = 0U; i < IPCAM_TCP_RETRY_MAX; i++) {
        if (tcp_connect_and_start_stream() == IPCAM_OK) break;
        if (i < IPCAM_TCP_RETRY_MAX - 1U) {
            LOG_W(TAG, "TCP retry %lu/%u in %ums",
                  (unsigned long)(i + 1U), IPCAM_TCP_RETRY_MAX,
                  IPCAM_TCP_RETRY_INTERVAL_MS);
            conn_delay_with_heartbeat(IPCAM_TCP_RETRY_INTERVAL_MS);
        }
    }

    if (s_net.state != NET_STATE_STREAMING) {
        LOG_E(TAG, "TCP connect failed after %u retries, going offline",
              IPCAM_TCP_RETRY_MAX);
        s_net.state = NET_STATE_OFFLINE;
        return IPCAM_ERR_TIMEOUT;
    }

    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * net_send_frame
 * 发送一帧 MJPEG multipart 数据
 * 格式：
 *   --frame\r\n
 *   Content-Type: image/jpeg\r\n
 *   Content-Length: <size>\r\n
 *   \r\n
 *   <JPEG data>
 * ----------------------------------------------------------------------- */
ipcam_status_t net_send_frame(const uint8_t *jpeg, uint32_t size)
{
    if (!s_net.initialized)           return IPCAM_ERR_NOT_INIT;
    if (s_net.state != NET_STATE_STREAMING) return IPCAM_ERR_IO;
    if (jpeg == NULL || size == 0U)   return IPCAM_ERR_INVALID;

    if (xSemaphoreTake(s_net.tx_mutex, pdMS_TO_TICKS(100U)) != pdTRUE) {
        return IPCAM_ERR_BUSY;
    }

    /* 构造 multipart 帧头 */
    char part_hdr[128];
    int hlen = snprintf(part_hdr, sizeof(part_hdr),
        "--frame\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: %lu\r\n"
        "\r\n",
        (unsigned long)size);

    ipcam_status_t ret = IPCAM_OK;

    if (hlen > 0) {
        /* 发送帧头 */
        uint32_t total = (uint32_t)hlen + size;
        char send_cmd[32];
        snprintf(send_cmd, sizeof(send_cmd), "AT+CIPSEND=%lu\r\n", (unsigned long)total);
        uart_rx_flush();
        at_send(send_cmd);

        if (!at_wait_prompt(AT_PROMPT_TIMEOUT_MS)) {
            LOG_W(TAG, "net_send_frame: no prompt");
            ret = IPCAM_ERR_TIMEOUT;
            goto done;
        }

        /* 发送帧头 + JPEG 数据（合并为一次 CIPSEND） */
        UART_WriteBlocking(WIFI_UART, (const uint8_t *)part_hdr, (uint32_t)hlen);
        UART_WriteBlocking(WIFI_UART, jpeg, size);

        if (!at_wait_response("SEND OK", AT_SEND_TIMEOUT_MS, NULL, 0U)) {
            LOG_W(TAG, "net_send_frame: SEND OK timeout");
            ret = IPCAM_ERR_TIMEOUT;
            goto done;
        }
    }

done:
    xSemaphoreGive(s_net.tx_mutex);

    if (ret != IPCAM_OK) {
        /* 发送失败，标记 TCP 断开，触发重连 */
        s_net.state = NET_STATE_WIFI_CONNECTED;
        s_net.http_header_sent = false;
    }
    return ret;
}

/* -----------------------------------------------------------------------
 * net_send_snapshot
 * 上传照片到服务器（HTTP POST /snapshot）
 * ----------------------------------------------------------------------- */
ipcam_status_t net_send_snapshot(const uint8_t *jpeg, uint32_t size, bool sd_failed)
{
    if (!s_net.initialized) return IPCAM_ERR_NOT_INIT;
    if (jpeg == NULL || size == 0U) return IPCAM_ERR_INVALID;

    /* 若当前不在推流状态，尝试重连 */
    if (s_net.state != NET_STATE_STREAMING) {
        LOG_W(TAG, "net_send_snapshot: not streaming, attempting reconnect");
        if (net_connect() != IPCAM_OK) {
            return IPCAM_ERR_IO;
        }
    }

    if (xSemaphoreTake(s_net.tx_mutex, pdMS_TO_TICKS(500U)) != pdTRUE) {
        return IPCAM_ERR_BUSY;
    }

    /* 构造 HTTP POST 请求（带 HMAC 认证） */
    char http_req[384];
    uint32_t ts_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    char hmac_hex[HMAC_HEX_LEN];
    net_auth_sign("POST", "/snapshot", ts_ms, NULL, 0U, hmac_hex);

    int hlen = snprintf(http_req, sizeof(http_req),
        "POST /snapshot HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: image/jpeg\r\n"
        "Content-Length: %lu\r\n"
        "X-SD-Failed: %s\r\n"
        "X-Timestamp: %lu\r\n"
        "X-HMAC-SHA256: %s\r\n"
        "\r\n",
        s_net.cfg.server_ip,
        s_net.cfg.server_port,
        (unsigned long)size,
        sd_failed ? "true" : "false",
        (unsigned long)ts_ms,
        hmac_hex);

    ipcam_status_t ret = IPCAM_OK;

    if (hlen > 0) {
        uint32_t total = (uint32_t)hlen + size;
        char send_cmd[32];
        snprintf(send_cmd, sizeof(send_cmd), "AT+CIPSEND=%lu\r\n", (unsigned long)total);
        uart_rx_flush();
        at_send(send_cmd);

        if (!at_wait_prompt(AT_PROMPT_TIMEOUT_MS)) {
            ret = IPCAM_ERR_TIMEOUT;
            goto snap_done;
        }

        UART_WriteBlocking(WIFI_UART, (const uint8_t *)http_req, (uint32_t)hlen);
        UART_WriteBlocking(WIFI_UART, jpeg, size);

        if (!at_wait_response("SEND OK", AT_SEND_TIMEOUT_MS * 3U, NULL, 0U)) {
            ret = IPCAM_ERR_TIMEOUT;
        }
    }

snap_done:
    xSemaphoreGive(s_net.tx_mutex);
    if (ret == IPCAM_OK) {
        LOG_I(TAG, "Snapshot uploaded (%lu bytes, sd_failed=%d)",
              (unsigned long)size, (int)sd_failed);
    }
    return ret;
}

/* -----------------------------------------------------------------------
 * net_send_status
 * 上报系统状态 JSON（HTTP POST /status）
 * ----------------------------------------------------------------------- */
ipcam_status_t net_send_status(const sys_status_t *status)
{
    if (!s_net.initialized || status == NULL) return IPCAM_ERR_INVALID;
    if (s_net.state != NET_STATE_STREAMING)   return IPCAM_ERR_IO;

    /* 序列化为 JSON */
    static const char * const net_state_str[] = {
        "idle", "wifi_connecting", "wifi_connected",
        "tcp_connecting", "streaming", "offline", "error"
    };
    uint32_t ns = (uint32_t)status->net_state;
    if (ns >= 7U) ns = 6U;

    char json[256];
    int jlen = snprintf(json, sizeof(json),
        "{\"net\":\"%s\",\"fps\":%u,\"sd_free_mb\":%lu,"
        "\"uptime\":%lu,\"drops\":%lu,"
        "\"cam\":%s,\"sd\":%s,\"sd_low\":%s}\r\n",
        net_state_str[ns],
        status->fps_current,
        (unsigned long)status->sd_free_mb,
        (unsigned long)status->uptime_sec,
        (unsigned long)status->drop_count,
        status->cam_available ? "true" : "false",
        status->sd_available  ? "true" : "false",
        status->sd_low_space  ? "true" : "false");

    if (jlen <= 0) return IPCAM_ERR;

    if (xSemaphoreTake(s_net.tx_mutex, pdMS_TO_TICKS(200U)) != pdTRUE) {
        return IPCAM_ERR_BUSY;
    }

    char http_req[256];
    uint32_t ts_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    char hmac_hex[HMAC_HEX_LEN];
    net_auth_sign("POST", "/status", ts_ms, NULL, 0U, hmac_hex);

    int hlen = snprintf(http_req, sizeof(http_req),
        "POST /status HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "Content-Type: application/json\r\n"
        "Content-Length: %d\r\n"
        "X-Timestamp: %lu\r\n"
        "X-HMAC-SHA256: %s\r\n"
        "\r\n",
        s_net.cfg.server_ip, s_net.cfg.server_port, jlen,
        (unsigned long)ts_ms, hmac_hex);

    ipcam_status_t ret = IPCAM_OK;

    if (hlen > 0) {
        uint32_t total = (uint32_t)hlen + (uint32_t)jlen;
        char send_cmd[32];
        snprintf(send_cmd, sizeof(send_cmd), "AT+CIPSEND=%lu\r\n", (unsigned long)total);
        uart_rx_flush();
        at_send(send_cmd);

        if (at_wait_prompt(AT_PROMPT_TIMEOUT_MS)) {
            UART_WriteBlocking(WIFI_UART, (const uint8_t *)http_req, (uint32_t)hlen);
            UART_WriteBlocking(WIFI_UART, (const uint8_t *)json, (uint32_t)jlen);
            at_wait_response("SEND OK", AT_SEND_TIMEOUT_MS, NULL, 0U);
        } else {
            ret = IPCAM_ERR_TIMEOUT;
        }
    }

    xSemaphoreGive(s_net.tx_mutex);
    return ret;
}

/* -----------------------------------------------------------------------
 * 私有：从 JSON 缓冲区解析指定键的整数值
 * @param buf      JSON 字符串
 * @param key      键（含引号和冒号，如 "\"quality\":"）
 * @param key_len  键长度（sizeof(key)-1）
 * @param def_val  解析失败时的默认值
 * @return 解析出的整数，键不存在或格式非法时返回 def_val
 * ----------------------------------------------------------------------- */
static long json_parse_int(const char *buf, const char *key,
                           size_t key_len, long def_val)
{
    const char *p = strstr(buf, key);
    if (p == NULL) {
        return def_val;
    }
    p += key_len;               /* 跳过键，指向值起始 */
    while (*p == ' ') p++;       /* 跳过可能的空格 */

    char *endp = NULL;
    long  val  = strtol(p, &endp, 10);
    if (endp == p) {             /* 没有解析到任何数字 */
        return def_val;
    }
    return val;
}

/* -----------------------------------------------------------------------
 * net_recv_cmd
 * 非阻塞检查是否有服务器下发的命令
 * 命令格式（JSON）：
 *   {"cmd":"snapshot","quality":85,"width":1280,"height":720}
 *   {"cmd":"set_fps","fps":15}
 *   {"cmd":"set_quality","quality":75}
 *   {"cmd":"reboot"}
 * ----------------------------------------------------------------------- */
ipcam_status_t net_recv_cmd(ipcam_cmd_t *cmd)
{
    if (cmd == NULL) return IPCAM_ERR_INVALID;
    cmd->type = CMD_NONE;

    if (!s_cmd_pending) {
        return IPCAM_ERR_NOT_FOUND;
    }

    /* 加锁后将命令拷贝到本地缓冲区再解析，避免解析期间
     * task_net_send 写入新命令造成数据撕裂 */
    char local_buf[IPCAM_CMD_BUF_SIZE];
    if (s_cmd_mutex == NULL ||
        xSemaphoreTake(s_cmd_mutex, pdMS_TO_TICKS(50U)) != pdTRUE) {
        return IPCAM_ERR_BUSY;
    }
    strncpy(local_buf, s_cmd_buf, IPCAM_CMD_BUF_SIZE - 1U);
    local_buf[IPCAM_CMD_BUF_SIZE - 1U] = '\0';
    s_cmd_pending = false;
    xSemaphoreGive(s_cmd_mutex);

    const char *buf = local_buf;

    /* 解析 "cmd" 字段值起始位置 */
    const char *cmd_val = strstr(buf, JSON_KEY_CMD);
    if (cmd_val == NULL) {
        return IPCAM_ERR_NOT_FOUND;
    }
    cmd_val += sizeof(JSON_KEY_CMD) - 1U;  /* 跳过 "cmd": */
    while (*cmd_val == ' ' || *cmd_val == '"') cmd_val++;

    if (strncmp(cmd_val, "snapshot", 8U) == 0) {
        cmd->type    = CMD_SNAPSHOT;
        cmd->quality = (uint8_t)json_parse_int(buf, JSON_KEY_QUALITY,
                                               sizeof(JSON_KEY_QUALITY) - 1U, 85);
        cmd->width   = (uint16_t)json_parse_int(buf, JSON_KEY_WIDTH,
                                                sizeof(JSON_KEY_WIDTH) - 1U, 1280);
        cmd->height  = (uint16_t)json_parse_int(buf, JSON_KEY_HEIGHT,
                                                sizeof(JSON_KEY_HEIGHT) - 1U, 720);

    } else if (strncmp(cmd_val, "set_fps", 7U) == 0) {
        cmd->type = CMD_SET_FPS;
        cmd->fps  = (uint8_t)json_parse_int(buf, JSON_KEY_FPS,
                                            sizeof(JSON_KEY_FPS) - 1U, 15);

    } else if (strncmp(cmd_val, "set_quality", 11U) == 0) {
        cmd->type    = CMD_SET_QUALITY;
        cmd->quality = (uint8_t)json_parse_int(buf, JSON_KEY_QUALITY,
                                               sizeof(JSON_KEY_QUALITY) - 1U, 75);

    } else if (strncmp(cmd_val, "reboot", 6U) == 0) {
        cmd->type = CMD_REBOOT;
    }

    if (cmd->type != CMD_NONE) {
        LOG_I(TAG, "Command parsed: type=%d", (int)cmd->type);
        return IPCAM_OK;
    }
    return IPCAM_ERR_NOT_FOUND;
}

/* -----------------------------------------------------------------------
 * net_reset_wifi_module
 * ----------------------------------------------------------------------- */
void net_reset_wifi_module(void)
{
    LOG_I(TAG, "WiFi module hardware reset");
    GPIO_PinWrite(WIFI_RST_GPIO, WIFI_RST_PIN, 0U);  /* 拉低 RST */
    vTaskDelay(pdMS_TO_TICKS(100U));                  /* 保持 100ms */
    GPIO_PinWrite(WIFI_RST_GPIO, WIFI_RST_PIN, 1U);  /* 释放 RST */
    vTaskDelay(pdMS_TO_TICKS(500U));                  /* 等待模块启动 */
}

/* -----------------------------------------------------------------------
 * net_get_state / net_is_wifi_connected / net_is_streaming
 * ----------------------------------------------------------------------- */
net_state_t net_get_state(void)
{
    return s_net.state;
}

bool net_is_wifi_connected(void)
{
    return (s_net.state == NET_STATE_WIFI_CONNECTED ||
            s_net.state == NET_STATE_TCP_CONNECTING  ||
            s_net.state == NET_STATE_STREAMING);
}

bool net_is_streaming(void)
{
    return (s_net.state == NET_STATE_STREAMING);
}

/* -----------------------------------------------------------------------
 * net_tick
 * 每秒由 sys_manager_task 调用，纯非阻塞：只做状态检测和标记，
 * 不在此函数内执行任何 vTaskDelay 或长时间 AT 指令等待。
 *
 * 重连动作由 task_net_send 在检测到状态变化后自行触发。
 * ----------------------------------------------------------------------- */
void net_tick(void)
{
    if (!s_net.initialized) return;

    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    switch (s_net.state) {

    case NET_STATE_STREAMING:
        /* 快速检查 TCP 活跃性：只发命令，不等响应（响应由 task_net_send 处理）
         * 若连接断开，at_wait_response 内部会检测到 "CLOSED" 并反映到状态 */
        s_net.last_connected_ms = now_ms;
        break;

    case NET_STATE_WIFI_CONNECTED:
        /* TCP 断开后的重连间隔检查（避免频繁重试） */
        if ((now_ms - s_net.disconnect_start_ms) >= IPCAM_TCP_RETRY_INTERVAL_MS) {
            if (s_net.tcp_retry_count >= IPCAM_TCP_RETRY_MAX) {
                LOG_E(TAG, "TCP reconnect exhausted, going offline");
                s_net.state = NET_STATE_OFFLINE;
                s_net.disconnect_start_ms = now_ms;
            }
            /* 重连动作由 task_net_send 检测状态后触发，此处仅计时 */
        }
        break;

    case NET_STATE_OFFLINE:
    case NET_STATE_IDLE: {
        uint32_t offline_ms = now_ms - s_net.disconnect_start_ms;

        /* 60s 无连接：标记需要硬件复位，由 task_net_send 执行 */
        if (offline_ms >= IPCAM_WIFI_DEAD_TIMEOUT_MS) {
            LOG_E(TAG, "WiFi dead for %lus, will trigger hardware reset",
                  (unsigned long)(offline_ms / 1000U));
            /* 重置计时，task_net_send 检测到 IDLE 状态会执行复位和重连 */
            s_net.disconnect_start_ms = now_ms;
            s_net.wifi_retry_count    = 0U;
        }
        break;
    }

    default:
        break;
    }
}
