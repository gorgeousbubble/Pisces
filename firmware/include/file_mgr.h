#ifndef FILE_MGR_H
#define FILE_MGR_H

/**
 * @file file_mgr.h
 * @brief SD 卡文件管理器接口（基于 FatFs）
 *
 * 管理录像文件（MJPEG）和照片文件（JPEG）的创建、写入和查询。
 */

#include "ipcam_types.h"
#include <stdint.h>
#include <stdbool.h>

/* -----------------------------------------------------------------------
 * 文件信息结构体（用于列表查询）
 * ----------------------------------------------------------------------- */
#define FM_FILENAME_MAX_LEN   32U
#define FM_MAX_FILE_LIST      64U   /**< 单次查询最多返回的文件数 */

typedef struct {
    char     filename[FM_FILENAME_MAX_LEN];
    uint32_t timestamp;    /**< Unix 时间戳（秒），从文件名解析 */
    uint32_t size_bytes;
} fm_file_info_t;

/* -----------------------------------------------------------------------
 * 函数声明
 * ----------------------------------------------------------------------- */

/**
 * @brief 初始化文件管理器（挂载 FAT32，检测 SD 卡）
 * @return IPCAM_OK / IPCAM_ERR_IO（SD 卡不可用）
 */
ipcam_status_t fm_init(void);

/**
 * @brief 检查 SD 卡是否可用
 */
bool fm_is_sd_available(void);

/**
 * @brief 开始录像（创建新的 REC_YYYYMMDD_HHMMSS.mjpeg 文件）
 * @return IPCAM_OK / IPCAM_ERR_FULL（空间不足）/ IPCAM_ERR_IO
 */
ipcam_status_t fm_start_recording(void);

/**
 * @brief 停止录像（写入结束标记并关闭文件）
 */
void fm_stop_recording(void);

/**
 * @brief 写入一帧 JPEG 数据到当前录像文件
 *        格式：[4B 小端 size][JPEG data]
 * @param data  JPEG 数据
 * @param size  数据字节数
 * @return IPCAM_OK / IPCAM_ERR_IO / IPCAM_ERR_NOT_INIT
 */
ipcam_status_t fm_write_frame(const uint8_t *data, uint32_t size);

/**
 * @brief 保存一张照片
 * @param data      JPEG 数据
 * @param size      数据字节数
 * @param out_path  输出：实际保存的文件路径（缓冲区至少 FM_FILENAME_MAX_LEN 字节）
 * @return IPCAM_OK / IPCAM_ERR_IO
 */
ipcam_status_t fm_save_snapshot(const uint8_t *data, uint32_t size, char *out_path);

/**
 * @brief 查询 SD 卡剩余空间
 * @param[out] free_mb  剩余空间（MB）
 * @return IPCAM_OK / IPCAM_ERR_IO
 */
ipcam_status_t fm_get_free_space(uint32_t *free_mb);

/**
 * @brief 列出录像文件
 * @param[out] list   文件信息数组（调用方分配）
 * @param[out] count  实际返回的文件数
 * @param      max    list 数组容量
 * @return IPCAM_OK / IPCAM_ERR_IO
 */
ipcam_status_t fm_list_recordings(fm_file_info_t *list, uint32_t *count, uint32_t max);

/**
 * @brief 检查当前录像文件是否需要轮转（≥ 3.9GB）
 *        若需要则自动关闭并新建文件。
 * @return IPCAM_OK / IPCAM_ERR_IO
 */
ipcam_status_t fm_check_rotate(void);

/**
 * @brief 获取当前录像文件大小（字节）
 */
uint64_t fm_get_current_file_size(void);

/**
 * @brief 获取连续低速写入秒数（需求 3.8 监控）
 *        返回值 >= WRITE_RATE_SLOW_THRESHOLD_SECS 时应触发告警
 */
uint32_t fm_get_slow_write_secs(void);

#endif /* FILE_MGR_H */
