#ifndef SYS_MANAGER_H
#define SYS_MANAGER_H

/**
 * @file sys_manager.h
 * @brief 系统管理器接口
 *
 * 负责看门狗喂狗、任务心跳监控、状态维护和错误恢复协调。
 */

#include "ipcam_types.h"
#include <stdbool.h>

/* -----------------------------------------------------------------------
 * 任务心跳 ID（每个任务对应一个 ID）
 * ----------------------------------------------------------------------- */
typedef enum {
    HEARTBEAT_CAM_CAPTURE = 0,
    HEARTBEAT_NET_SEND,
    HEARTBEAT_FILE_WRITE,
    HEARTBEAT_CMD_HANDLER,
    HEARTBEAT_COUNT,          /**< 心跳 ID 总数，必须最后 */
} heartbeat_id_t;

/* 心跳超时阈值（ms），超过此时间未更新则触发告警 */
#define HEARTBEAT_TIMEOUT_MS   3000U

/* -----------------------------------------------------------------------
 * 复位原因（保存到 Flash，掉电保持）
 * ----------------------------------------------------------------------- */
typedef enum {
    RESET_REASON_UNKNOWN    = 0x00,
    RESET_REASON_WATCHDOG   = 0x01,
    RESET_REASON_SOFT       = 0x02,
    RESET_REASON_CAM_FAIL   = 0x03,
    RESET_REASON_POWER_ON   = 0x04,
} reset_reason_t;

/* -----------------------------------------------------------------------
 * 函数声明
 * ----------------------------------------------------------------------- */

/**
 * @brief 初始化系统管理器（看门狗、心跳数组、状态结构体）
 * @return IPCAM_OK 或错误码
 */
ipcam_status_t sys_manager_init(void);

/**
 * @brief 系统管理器 FreeRTOS 任务入口
 *        优先级：低(2)，每 1 秒执行一次
 */
void sys_manager_task(void *param);

/**
 * @brief 看门狗喂狗任务入口（最高优先级，独立任务）
 */
void sys_watchdog_task(void *param);

/**
 * @brief 更新指定任务的心跳时间戳（在各任务主循环中调用）
 * @param id  任务心跳 ID
 */
void sys_heartbeat_update(heartbeat_id_t id);

/**
 * @brief 获取当前系统状态快照
 * @param[out] status  输出状态结构体
 */
void sys_get_status(sys_status_t *status);

/**
 * @brief 执行软复位（记录原因后触发 NVIC_SystemReset）
 * @param reason  复位原因
 */
void sys_soft_reset(reset_reason_t reason);

/**
 * @brief 读取上次复位原因（从 Flash 读取）
 * @return 上次复位原因
 */
reset_reason_t sys_get_last_reset_reason(void);

/**
 * @brief 原子递增 Drop_Counter
 */
void sys_drop_counter_inc(void);

/**
 * @brief 读取 Drop_Counter 当前值
 */
uint32_t sys_drop_counter_get(void);

/**
 * @brief 获取系统运行时间（秒）
 */
uint32_t sys_get_uptime_sec(void);

#endif /* SYS_MANAGER_H */
