#ifndef CAM_DRIVER_H
#define CAM_DRIVER_H

/**
 * @file cam_driver.h
 * @brief OV2640 摄像头驱动接口
 *
 * 通过 SCCB(I2C) 配置 OV2640，DVP 接口接收 JPEG 数据流。
 * OV2640 片上 JPEG 编码器直接输出 JPEG，MCU 无需软件编码。
 */

#include "ipcam_types.h"
#include "ipcam_config.h"

/* -----------------------------------------------------------------------
 * 摄像头分辨率枚举
 * ----------------------------------------------------------------------- */
typedef enum {
    CAM_RES_VGA    = 0,   /**< 640×480  - 视频流模式 */
    CAM_RES_HD720P = 1,   /**< 1280×720 - 拍照模式   */
} cam_resolution_t;

/* -----------------------------------------------------------------------
 * 摄像头配置结构体
 * ----------------------------------------------------------------------- */
typedef struct {
    cam_resolution_t resolution;
    uint8_t          jpeg_quality;  /**< 50–95，映射到 OV2640 QS 寄存器 */
    uint8_t          target_fps;    /**< 目标帧率 1–30 */
} cam_config_t;

/* -----------------------------------------------------------------------
 * 函数声明
 * ----------------------------------------------------------------------- */

/**
 * @brief 初始化摄像头驱动（I2C + DVP + DMA + XCLK）
 * @param cfg  摄像头配置，NULL 则使用默认配置
 * @return IPCAM_OK 或错误码
 */
ipcam_status_t cam_init(const cam_config_t *cfg);

/**
 * @brief 启动连续采集
 * @return IPCAM_OK 或错误码
 */
ipcam_status_t cam_start_capture(void);

/**
 * @brief 停止采集（低功耗待机）
 */
void cam_stop_capture(void);

/**
 * @brief 获取一帧 JPEG 数据（阻塞，带超时）
 * @param[out] frame       帧描述符（data 指针指向内部 DMA 缓冲区，使用完毕后调用 cam_release_frame）
 * @param      timeout_ms  超时时间（ms），0 = 不等待
 * @return IPCAM_OK / IPCAM_ERR_TIMEOUT / IPCAM_ERR_NOT_INIT
 */
ipcam_status_t cam_get_frame(ipcam_frame_t *frame, uint32_t timeout_ms);

/**
 * @brief 释放帧缓冲区（通知驱动可以复用该缓冲区）
 * @param frame  由 cam_get_frame 返回的帧
 */
void cam_release_frame(const ipcam_frame_t *frame);

/**
 * @brief 重新初始化摄像头（用于错误恢复）
 * @return IPCAM_OK 或错误码
 */
ipcam_status_t cam_reinit(void);

/**
 * @brief 切换分辨率（用于拍照模式切换）
 * @param res  目标分辨率
 * @return IPCAM_OK 或错误码
 */
ipcam_status_t cam_set_resolution(cam_resolution_t res);

/**
 * @brief 设置 JPEG 质量因子
 * @param quality  50–95
 * @return IPCAM_OK 或 IPCAM_ERR_INVALID
 */
ipcam_status_t cam_set_quality(uint8_t quality);

/**
 * @brief 获取累计丢帧计数
 */
uint32_t cam_get_drop_count(void);

/**
 * @brief 获取当前实际帧率（最近 1 秒统计）
 */
uint8_t cam_get_fps(void);

/**
 * @brief 帧采集任务体（在 task_cam_capture 主循环中调用）
 *
 * 执行一次完整的帧采集（软件轮询 DVP 信号），
 * 采集成功后通过内部信号量通知 cam_get_frame。
 * 此函数会阻塞直到一帧采集完成或超时。
 */
void cam_capture_task_body(void);

#endif /* CAM_DRIVER_H */
