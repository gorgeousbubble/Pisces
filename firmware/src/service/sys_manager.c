/**
 * @file sys_manager.c
 * @brief 系统管理器实现
 *
 * 看门狗配置、任务心跳监控、Drop_Counter、状态维护、软复位。
 */

#include "sys_manager.h"
#include "log.h"
#include "board.h"
#include "net_stack.h"
#include "cam_driver.h"
#include "file_mgr.h"
#include "net_auth.h"
#include "ipcam_config.h"
#include "fsl_wdog.h"
#include "FreeRTOS.h"
#include "task.h"
#include "atomic.h"   /* FreeRTOS atomic ops */
#include <string.h>

#define TAG "SYS"

/* -----------------------------------------------------------------------
 * 私有数据
 * ----------------------------------------------------------------------- */

/** 各任务最后一次心跳时间戳（ms） */
static volatile uint32_t s_heartbeat_ts[HEARTBEAT_COUNT];

/** Drop_Counter（原子操作） */
static volatile uint32_t s_drop_counter = 0U;

/** 系统启动时间戳（ms） */
static uint32_t s_boot_tick_ms = 0U;

/** 上次复位原因（存储在 VBAT 寄存器，掉电保持） */
#define RESET_REASON_REG   (*(volatile uint32_t *)0x4003E000U)  /* LLWU_FILT1 借用 */

/* -----------------------------------------------------------------------
 * 看门狗初始化
 * ----------------------------------------------------------------------- */
static void wdog_init(void)
{
    wdog_config_t cfg;
    WDOG_GetDefaultConfig(&cfg);
    cfg.enableWdog    = true;
    cfg.timeoutValue  = 0x0500U;  /* 约 5 秒（LPO 1kHz，分频后） */
    cfg.clockSource   = kWDOG_LpoClockSource;
    cfg.prescaler     = kWDOG_ClockPrescalerDivide1;
    cfg.enableUpdate  = true;
    cfg.workMode.enableWait  = true;
    cfg.workMode.enableStop  = false;
    cfg.workMode.enableDebug = false;
    WDOG_Init(WDOG, &cfg);
    LOG_I(TAG, "Watchdog initialized, timeout=%ums", WDG_TIMEOUT_MS);
}

/* -----------------------------------------------------------------------
 * sys_manager_init
 * ----------------------------------------------------------------------- */
ipcam_status_t sys_manager_init(void)
{
    memset((void *)s_heartbeat_ts, 0, sizeof(s_heartbeat_ts));
    s_drop_counter  = 0U;
    s_boot_tick_ms  = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    wdog_init();

    reset_reason_t last = sys_get_last_reset_reason();
    LOG_I(TAG, "Last reset reason: %d", (int)last);

    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * sys_watchdog_task（最高优先级，独立任务）
 * ----------------------------------------------------------------------- */
void sys_watchdog_task(void *param)
{
    (void)param;
    const TickType_t period = pdMS_TO_TICKS(WDG_FEED_INTERVAL_MS);

    for (;;) {
        WDOG_Refresh(WDOG);
        vTaskDelay(period);
    }
}

/* -----------------------------------------------------------------------
 * sys_manager_task（低优先级，每秒执行）
 * ----------------------------------------------------------------------- */
void sys_manager_task(void *param)
{
    (void)param;
    const TickType_t period = pdMS_TO_TICKS(1000U);
    uint32_t status_report_counter = 0U;

    for (;;) {
        uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

        /* --- 检查各任务心跳 --- */
        for (int i = 0; i < (int)HEARTBEAT_COUNT; i++) {
            uint32_t last_ts = s_heartbeat_ts[i];
            if (last_ts != 0U && (now_ms - last_ts) > HEARTBEAT_TIMEOUT_MS) {
                LOG_E(TAG, "Task heartbeat timeout: id=%d, last=%lu, now=%lu",
                      i, (unsigned long)last_ts, (unsigned long)now_ms);
                /* 心跳超时：触发软复位 */
                sys_soft_reset(RESET_REASON_WATCHDOG);
            }
        }

        /* --- 网络层周期维护 --- */
        net_tick();

        /* --- 定时上报状态 --- */
        status_report_counter++;
        if (status_report_counter >= (IPCAM_STATUS_REPORT_INTERVAL_MS / 1000U)) {
            status_report_counter = 0U;
            sys_status_t st;
            sys_get_status(&st);
            net_send_status(&st);
        }

        /* --- 写入速率告警检查（需求 3.8）--- */
        if (fm_get_slow_write_secs() >= 5U) {
            LOG_W(TAG, "SD write performance degraded for %lu seconds",
                  (unsigned long)fm_get_slow_write_secs());
            /* 即时上报（不等定时周期） */
            sys_status_t st;
            sys_get_status(&st);
            net_send_status(&st);
        }

        /* --- LED 状态指示 --- */
        if (net_is_streaming()) {
            LED_STATUS_TOGGLE();   /* 流媒体正常：绿灯闪烁 */
            LED_ERROR_OFF();
        } else if (net_is_wifi_connected()) {
            LED_STATUS_ON();       /* WiFi 已连接但未推流：绿灯常亮 */
            LED_ERROR_OFF();
        } else {
            LED_STATUS_OFF();
            LED_ERROR_ON();        /* 无网络：红灯常亮 */
        }

        vTaskDelay(period);
    }
}

/* -----------------------------------------------------------------------
 * sys_heartbeat_update
 * ----------------------------------------------------------------------- */
void sys_heartbeat_update(heartbeat_id_t id)
{
    if (id < HEARTBEAT_COUNT) {
        s_heartbeat_ts[id] = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    }
}

/* -----------------------------------------------------------------------
 * sys_get_status
 * ----------------------------------------------------------------------- */
void sys_get_status(sys_status_t *status)
{
    if (status == NULL) {
        return;
    }
    status->net_state    = net_get_state();
    status->fps_current  = cam_get_fps();
    status->drop_count   = sys_drop_counter_get();
    status->uptime_sec   = sys_get_uptime_sec();
    status->cam_available = true;  /* 由 cam_driver 维护，此处简化 */
    status->sd_available  = fm_is_sd_available();

    uint32_t free_mb = 0U;
    fm_get_free_space(&free_mb);
    status->sd_free_mb  = free_mb;
    status->sd_low_space = (free_mb < IPCAM_SD_LOW_SPACE_MB);
}

/* -----------------------------------------------------------------------
 * sys_soft_reset
 * ----------------------------------------------------------------------- */
void sys_soft_reset(reset_reason_t reason)
{
    LOG_E(TAG, "Soft reset triggered, reason=%d", (int)reason);
    log_flush();

    /* 保存复位原因（借用 VBAT 寄存器，掉电保持） */
    RESET_REASON_REG = (uint32_t)reason;

    /* 触发 Cortex-M4 软复位 */
    NVIC_SystemReset();
}

/* -----------------------------------------------------------------------
 * sys_get_last_reset_reason
 * ----------------------------------------------------------------------- */
reset_reason_t sys_get_last_reset_reason(void)
{
    uint32_t val = RESET_REASON_REG;
    RESET_REASON_REG = (uint32_t)RESET_REASON_POWER_ON;  /* 清除 */
    if (val > (uint32_t)RESET_REASON_POWER_ON) {
        return RESET_REASON_UNKNOWN;
    }
    return (reset_reason_t)val;
}

/* -----------------------------------------------------------------------
 * Drop_Counter
 * ----------------------------------------------------------------------- */
void sys_drop_counter_inc(void)
{
    /* 使用 FreeRTOS 原子加（Cortex-M4 LDREX/STREX） */
    Atomic_Increment_u32((uint32_t *)&s_drop_counter);
}

uint32_t sys_drop_counter_get(void)
{
    return s_drop_counter;
}

/* -----------------------------------------------------------------------
 * sys_get_uptime_sec
 * ----------------------------------------------------------------------- */
uint32_t sys_get_uptime_sec(void)
{
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    return (now_ms - s_boot_tick_ms) / 1000U;
}
