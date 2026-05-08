#ifndef NET_STACK_H
#define NET_STACK_H

/**
 * @file net_stack.h
 * @brief WiFi 网络传输层接口（基于 ESP8266/ESP32 AT 指令）
 *
 * 管理 WiFi 连接、TCP 连接、MJPEG 视频流推送和命令接收。
 */

#include "ipcam_types.h"
#include "ipcam_config.h"

/* -----------------------------------------------------------------------
 * 函数声明
 * ----------------------------------------------------------------------- */

/**
 * @brief 初始化网络层（配置 UART，复位 WiFi 模块）
 * @param cfg  系统配置（读取 WiFi SSID/密码、服务器 IP/端口）
 * @return IPCAM_OK 或错误码
 */
ipcam_status_t net_init(const ipcam_config_t *cfg);

/**
 * @brief 执行完整连接流程（WiFi 关联 + TCP 连接）
 *        阻塞直到连接成功或超过最大重试次数。
 * @return IPCAM_OK / IPCAM_ERR_TIMEOUT（进入离线模式）
 */
ipcam_status_t net_connect(void);

/**
 * @brief 发送一帧 JPEG 视频数据（MJPEG-over-HTTP multipart）
 * @param jpeg  JPEG 数据
 * @param size  数据字节数
 * @return IPCAM_OK / IPCAM_ERR_TIMEOUT / IPCAM_ERR_IO
 */
ipcam_status_t net_send_frame(const uint8_t *jpeg, uint32_t size);

/**
 * @brief 上传一张照片到服务器
 * @param jpeg      JPEG 数据
 * @param size      数据字节数
 * @param sd_failed true = SD 卡写入失败，需在上传数据中附加标记
 * @return IPCAM_OK / IPCAM_ERR_TIMEOUT / IPCAM_ERR_IO
 */
ipcam_status_t net_send_snapshot(const uint8_t *jpeg, uint32_t size, bool sd_failed);

/**
 * @brief 上报系统状态 JSON 到服务器
 * @param status  当前系统状态
 * @return IPCAM_OK / IPCAM_ERR_IO
 */
ipcam_status_t net_send_status(const sys_status_t *status);

/**
 * @brief 尝试接收服务器下发的命令（非阻塞）
 * @param[out] cmd  解析出的命令，type=CMD_NONE 表示无命令
 * @return IPCAM_OK / IPCAM_ERR_NOT_FOUND（无命令）
 */
ipcam_status_t net_recv_cmd(ipcam_cmd_t *cmd);

/**
 * @brief 对 WiFi 模块执行硬件复位（拉低 RST 引脚 100ms）
 */
void net_reset_wifi_module(void);

/**
 * @brief 获取当前网络状态
 */
net_state_t net_get_state(void);

/**
 * @brief 检查 WiFi 是否已连接
 */
bool net_is_wifi_connected(void);

/**
 * @brief 检查 TCP 流是否已建立
 */
bool net_is_streaming(void);

/**
 * @brief 网络层周期性维护（在 sys_manager 任务中每秒调用）
 *        处理断线检测、重连逻辑、60s 无连接触发硬复位。
 */
void net_tick(void);

#endif /* NET_STACK_H */
