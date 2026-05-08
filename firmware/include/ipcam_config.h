#ifndef IPCAM_CONFIG_H
#define IPCAM_CONFIG_H

/**
 * @file ipcam_config.h
 * @brief 系统运行时配置结构体与默认值
 *
 * 配置从 SD 卡 config.ini 加载，失败时使用此处的默认值。
 */

#include <stdint.h>
#include <stdbool.h>

/* -----------------------------------------------------------------------
 * 编译期常量（不可通过 config.ini 修改）
 * ----------------------------------------------------------------------- */
#define IPCAM_VERSION_MAJOR      1U
#define IPCAM_VERSION_MINOR      0U
#define IPCAM_VERSION_PATCH      0U

#define IPCAM_FRAME_QUEUE_DEPTH  2U            /**< 编码帧队列深度 */
#define IPCAM_JPEG_BUF_SIZE      (40U * 1024U) /**< 单帧 JPEG 最大字节数 40KB */
#define IPCAM_LOG_BUF_SIZE       (4U * 1024U)  /**< 日志环形缓冲区大小 */
#define IPCAM_AT_BUF_SIZE        512U           /**< AT 指令收发缓冲区 */
#define IPCAM_CMD_BUF_SIZE       256U           /**< 服务器命令接收缓冲区 */

#define IPCAM_SD_LOW_SPACE_MB    50U            /**< SD 卡剩余空间告警阈值 (MB) */
#define IPCAM_FILE_MAX_SIZE_GB   3900000000ULL  /**< 单录像文件最大字节数 3.9GB */

#define IPCAM_WIFI_RETRY_MAX     5U             /**< WiFi 连接最大重试次数 */
#define IPCAM_WIFI_RETRY_INTERVAL_MS  10000U   /**< WiFi 重试间隔 10s */
#define IPCAM_TCP_RETRY_MAX      5U             /**< TCP 重连最大次数 */
#define IPCAM_TCP_RETRY_INTERVAL_MS   5000U    /**< TCP 重连间隔 5s */
#define IPCAM_TCP_CONNECT_TIMEOUT_MS  30000U   /**< TCP 初始连接超时 30s */
#define IPCAM_WIFI_CONNECT_TIMEOUT_MS 30000U   /**< WiFi 连接超时 30s */
#define IPCAM_WIFI_DEAD_TIMEOUT_MS    60000U   /**< WiFi 无连接触发硬复位阈值 60s */

#define IPCAM_CAM_FRAME_TIMEOUT_MS    66U      /**< 单帧采集超时 66ms (15fps) */
#define IPCAM_CAM_TIMEOUT_FRAMES      10U      /**< 连续超时帧数触发重初始化 */
#define IPCAM_CAM_REINIT_MAX          3U       /**< 摄像头重初始化最大次数 */

#define IPCAM_SNAPSHOT_TIMEOUT_MS     3000U    /**< 拍照命令超时 3s */
#define IPCAM_STATUS_REPORT_INTERVAL_MS 10000U /**< 状态上报间隔 10s */

/* -----------------------------------------------------------------------
 * 运行时配置结构体
 * ----------------------------------------------------------------------- */
#define IPCAM_SSID_MAX_LEN       32U
#define IPCAM_PASS_MAX_LEN       64U
#define IPCAM_IP_MAX_LEN         16U

typedef struct {
    /* WiFi 配置 */
    char     wifi_ssid[IPCAM_SSID_MAX_LEN];
    char     wifi_password[IPCAM_PASS_MAX_LEN];

    /* 服务器配置 */
    char     server_ip[IPCAM_IP_MAX_LEN];
    uint16_t server_port;

    /* 摄像头配置 */
    uint8_t  jpeg_quality;     /**< JPEG 质量因子 50–95 */
    uint8_t  target_fps;       /**< 目标帧率 1–30 */
} ipcam_config_t;

/* -----------------------------------------------------------------------
 * 默认配置值
 * ----------------------------------------------------------------------- */
#define IPCAM_DEFAULT_SERVER_IP      "192.168.1.100"
#define IPCAM_DEFAULT_SERVER_PORT    8554U
#define IPCAM_DEFAULT_JPEG_QUALITY   75U
#define IPCAM_DEFAULT_TARGET_FPS     15U
#define IPCAM_DEFAULT_WIFI_SSID      "MyHomeWiFi"
#define IPCAM_DEFAULT_WIFI_PASSWORD  ""

/* -----------------------------------------------------------------------
 * 全局配置实例（在 config_loader.c 中定义）
 * ----------------------------------------------------------------------- */
extern ipcam_config_t g_ipcam_config;

#endif /* IPCAM_CONFIG_H */
