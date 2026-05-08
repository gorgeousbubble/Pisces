#ifndef IPCAM_TYPES_H
#define IPCAM_TYPES_H

/**
 * @file ipcam_types.h
 * @brief 系统公共类型定义
 */

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* -----------------------------------------------------------------------
 * 通用状态码
 * ----------------------------------------------------------------------- */
typedef enum {
    IPCAM_OK              =  0,   /**< 成功 */
    IPCAM_ERR             = -1,   /**< 通用错误 */
    IPCAM_ERR_TIMEOUT     = -2,   /**< 超时 */
    IPCAM_ERR_NOMEM       = -3,   /**< 内存不足 */
    IPCAM_ERR_BUSY        = -4,   /**< 资源忙 */
    IPCAM_ERR_INVALID     = -5,   /**< 参数无效 */
    IPCAM_ERR_NOT_FOUND   = -6,   /**< 资源不存在 */
    IPCAM_ERR_IO          = -7,   /**< I/O 错误 */
    IPCAM_ERR_FULL        = -8,   /**< 缓冲区/存储已满 */
    IPCAM_ERR_NOT_INIT    = -9,   /**< 模块未初始化 */
    IPCAM_ERR_HW          = -10,  /**< 硬件故障 */
} ipcam_status_t;

/* -----------------------------------------------------------------------
 * 编码帧描述符（在队列中传递）
 * ----------------------------------------------------------------------- */
typedef struct {
    uint8_t  *data;          /**< JPEG 数据指针（指向静态缓冲区） */
    uint32_t  size;          /**< JPEG 数据字节数 */
    uint32_t  frame_id;      /**< 帧序号（单调递增） */
    uint32_t  timestamp_ms;  /**< 采集完成时间戳（系统 tick，ms） */
    bool      is_snapshot;   /**< true = 拍照帧，false = 普通视频帧 */
} ipcam_frame_t;

/* -----------------------------------------------------------------------
 * 系统运行状态（用于上报服务器）
 * ----------------------------------------------------------------------- */
typedef enum {
    NET_STATE_IDLE = 0,
    NET_STATE_WIFI_CONNECTING,
    NET_STATE_WIFI_CONNECTED,
    NET_STATE_TCP_CONNECTING,
    NET_STATE_STREAMING,
    NET_STATE_OFFLINE,
    NET_STATE_ERROR,
} net_state_t;

typedef struct {
    net_state_t  net_state;
    uint8_t      fps_current;
    uint32_t     sd_free_mb;
    uint32_t     uptime_sec;
    uint32_t     drop_count;
    bool         cam_available;
    bool         sd_available;
    bool         sd_low_space;
} sys_status_t;

/* -----------------------------------------------------------------------
 * 服务器下发命令
 * ----------------------------------------------------------------------- */
typedef enum {
    CMD_NONE = 0,
    CMD_SNAPSHOT,       /**< 拍照 */
    CMD_SET_FPS,        /**< 设置帧率 */
    CMD_SET_QUALITY,    /**< 设置 JPEG 质量 */
    CMD_REBOOT,         /**< 重启 MCU */
} ipcam_cmd_type_t;

typedef struct {
    ipcam_cmd_type_t type;
    uint8_t          quality;   /**< CMD_SNAPSHOT / CMD_SET_QUALITY 时有效 */
    uint8_t          fps;       /**< CMD_SET_FPS 时有效 */
    uint16_t         width;     /**< CMD_SNAPSHOT 时有效 */
    uint16_t         height;    /**< CMD_SNAPSHOT 时有效 */
} ipcam_cmd_t;

#endif /* IPCAM_TYPES_H */
