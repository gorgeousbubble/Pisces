/**
 * @file main.c
 * @brief 系统入口与 FreeRTOS 任务创建
 *
 * 初始化顺序：
 *   1. 板级初始化（时钟、引脚）
 *   2. 日志模块
 *   3. SD 卡 + 配置加载
 *   4. 摄像头驱动
 *   5. 网络层
 *   6. 系统管理器
 *   7. 创建所有 FreeRTOS 任务
 *   8. 启动调度器
 */

#include "board.h"
#include "log.h"
#include "config_loader.h"
#include "cam_driver.h"
#include "file_mgr.h"
#include "net_stack.h"
#include "net_auth.h"
#include "sys_manager.h"
#include "rtc_driver.h"
#include "ipcam_config.h"
#include "ipcam_types.h"
#include "FreeRTOS.h"
#include "task.h"
#include "queue.h"
#include "semphr.h"
#include <string.h>

#define TAG "MAIN"

/* -----------------------------------------------------------------------
 * 任务栈大小（单位：StackType_t，即 4 字节）
 * ----------------------------------------------------------------------- */
#define STACK_CAM_CAPTURE    (512U)   /* 2KB */
#define STACK_NET_SEND       (768U)   /* 3KB */
#define STACK_FILE_WRITE     (768U)   /* 3KB */
#define STACK_CMD_HANDLER    (512U)   /* 2KB */
#define STACK_SYS_MANAGER    (512U)   /* 2KB */
#define STACK_WATCHDOG       (256U)   /* 1KB */

/* -----------------------------------------------------------------------
 * 任务优先级
 * ----------------------------------------------------------------------- */
#define PRIO_WATCHDOG        (configMAX_PRIORITIES - 1U)  /* 最高 */
#define PRIO_CAM_CAPTURE     (configMAX_PRIORITIES - 2U)  /* 高   */
#define PRIO_NET_SEND        (configMAX_PRIORITIES - 3U)  /* 中   */
#define PRIO_FILE_WRITE      (configMAX_PRIORITIES - 3U)  /* 中   */
#define PRIO_CMD_HANDLER     (configMAX_PRIORITIES - 3U)  /* 中   */
#define PRIO_SYS_MANAGER     (configMAX_PRIORITIES - 4U)  /* 低   */

/* -----------------------------------------------------------------------
 * 编码帧队列（cam_capture -> net_send / file_write 共享）
 * ----------------------------------------------------------------------- */
static QueueHandle_t s_frame_queue_net  = NULL;  /**< 发往网络的帧队列 */
static QueueHandle_t s_frame_queue_file = NULL;  /**< 发往文件的帧队列 */

/* 拍照完成信号量（cmd_handler 等待，cam_capture 释放） */
static SemaphoreHandle_t s_snapshot_sem = NULL;
static ipcam_frame_t     s_snapshot_frame;

/* -----------------------------------------------------------------------
 * task_cam_capture
 * 调用 cam_capture_task_body 完成帧采集，再分发到网络/文件队列
 * ----------------------------------------------------------------------- */
static void task_cam_capture(void *param)
{
    (void)param;
    LOG_I(TAG, "task_cam_capture started");

    ipcam_status_t ret = cam_start_capture();
    if (ret != IPCAM_OK) {
        LOG_E(TAG, "cam_start_capture failed: %d", (int)ret);
        vTaskDelete(NULL);
        return;
    }

    uint32_t consecutive_timeout = 0U;

    for (;;) {
        sys_heartbeat_update(HEARTBEAT_CAM_CAPTURE);

        /* 执行一次帧采集（软件轮询，阻塞直到帧完成或超时） */
        cam_capture_task_body();

        /* 尝试获取就绪帧 */
        ipcam_frame_t frame;
        ret = cam_get_frame(&frame, 0U);  /* 非阻塞：帧已就绪则立即返回 */

        if (ret == IPCAM_ERR_TIMEOUT) {
            consecutive_timeout++;
            if (consecutive_timeout >= IPCAM_CAM_TIMEOUT_FRAMES) {
                LOG_W(TAG, "Consecutive timeout %lu frames, reinit camera",
                      (unsigned long)consecutive_timeout);
                ipcam_status_t ri = cam_reinit();
                if (ri != IPCAM_OK) {
                    sys_status_t st;
                    sys_get_status(&st);
                    st.cam_available = false;
                    net_send_status(&st);
                }
                consecutive_timeout = 0U;
            }
            continue;
        }

        if (ret != IPCAM_OK) {
            LOG_E(TAG, "cam_get_frame error: %d", (int)ret);
            continue;
        }

        consecutive_timeout = 0U;

        /* 拍照帧单独处理 */
        if (frame.is_snapshot) {
            memcpy(&s_snapshot_frame, &frame, sizeof(ipcam_frame_t));
            xSemaphoreGive(s_snapshot_sem);
            cam_release_frame(&frame);
            continue;
        }

        /* 分发到网络队列（满则丢帧） */
        if (xQueueSend(s_frame_queue_net, &frame, 0U) != pdTRUE) {
            sys_drop_counter_inc();
            LOG_W(TAG, "Net queue full, frame #%lu dropped",
                  (unsigned long)frame.frame_id);
            cam_release_frame(&frame);
        }

        /* 分发到文件队列（满则跳过，不计入 drop_counter） */
        if (xQueueSend(s_frame_queue_file, &frame, 0U) != pdTRUE) {
            LOG_D(TAG, "File queue full, frame #%lu skipped",
                  (unsigned long)frame.frame_id);
        }
    }
}

/* -----------------------------------------------------------------------
 * task_net_send
 * 从网络队列取帧，通过 WiFi 推送到服务器
 * ----------------------------------------------------------------------- */
static void task_net_send(void *param)
{
    (void)param;
    LOG_I(TAG, "task_net_send started");

    /* 等待网络连接建立 */
    while (!net_is_streaming()) {
        vTaskDelay(pdMS_TO_TICKS(500U));
    }

    for (;;) {
        sys_heartbeat_update(HEARTBEAT_NET_SEND);

        ipcam_frame_t frame;
        if (xQueueReceive(s_frame_queue_net, &frame, pdMS_TO_TICKS(200U)) != pdTRUE) {
            continue;
        }

        if (net_is_streaming()) {
            ipcam_status_t ret = net_send_frame(frame.data, frame.size);
            if (ret != IPCAM_OK) {
                LOG_W(TAG, "net_send_frame failed: %d, frame #%lu dropped",
                      (int)ret, (unsigned long)frame.frame_id);
                sys_drop_counter_inc();
            }
        }

        cam_release_frame(&frame);
    }
}

/* -----------------------------------------------------------------------
 * task_file_write
 * 从文件队列取帧，写入 SD 卡
 * ----------------------------------------------------------------------- */
static void task_file_write(void *param)
{
    (void)param;
    LOG_I(TAG, "task_file_write started");

    if (!fm_is_sd_available()) {
        LOG_W(TAG, "SD card not available, file write task idle");
        /* SD 卡不可用时任务保持运行但不写入 */
    }

    /* 开始录像 */
    ipcam_status_t ret = fm_start_recording();
    if (ret != IPCAM_OK) {
        LOG_W(TAG, "fm_start_recording failed: %d", (int)ret);
    }

    uint32_t write_fail_count = 0U;

    for (;;) {
        sys_heartbeat_update(HEARTBEAT_FILE_WRITE);

        ipcam_frame_t frame;
        if (xQueueReceive(s_frame_queue_file, &frame, pdMS_TO_TICKS(200U)) != pdTRUE) {
            continue;
        }

        if (!fm_is_sd_available()) {
            continue;
        }

        /* 检查文件轮转 */
        fm_check_rotate();

        /* 检查存储空间 */
        uint32_t free_mb = 0U;
        fm_get_free_space(&free_mb);
        if (free_mb < IPCAM_SD_LOW_SPACE_MB) {
            LOG_W(TAG, "SD card low space: %lu MB", (unsigned long)free_mb);
            fm_stop_recording();
            /* 告警已在 sys_manager 中处理 */
            continue;
        }

        ret = fm_write_frame(frame.data, frame.size);
        if (ret != IPCAM_OK) {
            write_fail_count++;
            LOG_W(TAG, "fm_write_frame failed: %d (frame #%lu, fail_count=%lu)",
                  (int)ret,
                  (unsigned long)frame.frame_id,
                  (unsigned long)write_fail_count);
        } else {
            write_fail_count = 0U;
        }
    }
}

/* -----------------------------------------------------------------------
 * task_cmd_handler
 * 处理来自服务器的控制命令
 * ----------------------------------------------------------------------- */
static void task_cmd_handler(void *param)
{
    (void)param;
    LOG_I(TAG, "task_cmd_handler started");

    for (;;) {
        sys_heartbeat_update(HEARTBEAT_CMD_HANDLER);

        ipcam_cmd_t cmd;
        ipcam_status_t ret = net_recv_cmd(&cmd);
        if (ret != IPCAM_OK || cmd.type == CMD_NONE) {
            vTaskDelay(pdMS_TO_TICKS(100U));
            continue;
        }

        LOG_I(TAG, "Received command: type=%d", (int)cmd.type);

        switch (cmd.type) {
        case CMD_SNAPSHOT: {
            /* 切换到高分辨率 */
            uint16_t w = (cmd.width  >= 1280U) ? cmd.width  : 1280U;
            uint16_t h = (cmd.height >= 720U)  ? cmd.height : 720U;
            uint8_t  q = (cmd.quality >= 80U)  ? cmd.quality : 80U;

            cam_set_resolution(CAM_RES_HD720P);
            cam_set_quality(q);

            /* 等待拍照帧（超时 3 秒） */
            bool sd_failed = false;
            if (xSemaphoreTake(s_snapshot_sem, pdMS_TO_TICKS(IPCAM_SNAPSHOT_TIMEOUT_MS)) == pdTRUE) {
                /* 保存到 SD 卡 */
                char path[FM_FILENAME_MAX_LEN];
                ipcam_status_t fm_ret = fm_save_snapshot(
                    s_snapshot_frame.data, s_snapshot_frame.size, path);
                if (fm_ret != IPCAM_OK) {
                    LOG_W(TAG, "Snapshot SD save failed: %d", (int)fm_ret);
                    sd_failed = true;
                }
                /* 上传到服务器 */
                net_send_snapshot(s_snapshot_frame.data, s_snapshot_frame.size, sd_failed);
            } else {
                LOG_W(TAG, "Snapshot timeout (no frame in %ums)", IPCAM_SNAPSHOT_TIMEOUT_MS);
            }

            /* 恢复 VGA 模式 */
            cam_set_resolution(CAM_RES_VGA);
            cam_set_quality(g_ipcam_config.jpeg_quality);
            break;
        }

        case CMD_SET_FPS:
            if (cmd.fps >= 1U && cmd.fps <= 30U) {
                g_ipcam_config.target_fps = cmd.fps;
                LOG_I(TAG, "FPS set to %u", cmd.fps);
            }
            break;

        case CMD_SET_QUALITY:
            if (cmd.quality >= 50U && cmd.quality <= 95U) {
                cam_set_quality(cmd.quality);
                g_ipcam_config.jpeg_quality = cmd.quality;
                LOG_I(TAG, "Quality set to %u", cmd.quality);
            }
            break;

        case CMD_REBOOT:
            LOG_W(TAG, "Reboot command received");
            sys_soft_reset(RESET_REASON_SOFT);
            break;

        default:
            LOG_W(TAG, "Unknown command type: %d", (int)cmd.type);
            break;
        }
    }
}

/* -----------------------------------------------------------------------
 * main
 * ----------------------------------------------------------------------- */
int main(void)
{
    /* 1. 板级初始化 */
    BOARD_InitClocks();
    BOARD_InitPins();

    /* 2. 日志模块（最先初始化，后续所有模块依赖它） */
    if (log_init() != IPCAM_OK) {
        /* 日志初始化失败，点亮错误 LED 后挂起 */
        LED_ERROR_ON();
        for (;;) { __asm("NOP"); }
    }

    LOG_I(TAG, "=== K64 IP Camera Firmware Starting ===");
    LOG_I(TAG, "Reset reason: %d", (int)sys_get_last_reset_reason());

    /* 3. 文件管理器（SD 卡挂载，为配置加载做准备） */
    ipcam_status_t ret = fm_init();
    if (ret != IPCAM_OK) {
        LOG_W(TAG, "SD card init failed (%d), local storage disabled", (int)ret);
    }

    /* 4. 加载配置文件 */
    ret = config_load();
    if (ret != IPCAM_OK) {
        LOG_W(TAG, "Config load failed, using defaults");
    }

    /* 5. RTC 初始化（在 SD 卡挂载后，确保 I2C 引脚已配置） */
    ret = rtc_init();
    if (ret != IPCAM_OK) {
        LOG_W(TAG, "RTC init failed (%d), timestamps may be inaccurate", (int)ret);
    }

    /* 5b. HMAC 认证初始化 */
    net_auth_init(NULL);  /* NULL = 使用默认密钥，生产环境在 config.ini [auth] key= 中配置 */

    /* 6. 摄像头驱动 */
    cam_config_t cam_cfg = {
        .resolution   = CAM_RES_VGA,
        .jpeg_quality = g_ipcam_config.jpeg_quality,
        .target_fps   = g_ipcam_config.target_fps,
    };
    ret = cam_init(&cam_cfg);
    if (ret != IPCAM_OK) {
        LOG_E(TAG, "Camera init failed (%d)", (int)ret);
        LED_ERROR_ON();
        /* 摄像头失败仍继续启动，允许远程诊断 */
    }

    /* 7. 网络层 */
    ret = net_init(&g_ipcam_config);
    if (ret != IPCAM_OK) {
        LOG_E(TAG, "Network init failed (%d)", (int)ret);
    }

    /* 8. 系统管理器 */
    ret = sys_manager_init();
    if (ret != IPCAM_OK) {
        LOG_E(TAG, "Sys manager init failed (%d)", (int)ret);
    }

    /* 9. 创建队列和信号量 */
    s_frame_queue_net  = xQueueCreate(IPCAM_FRAME_QUEUE_DEPTH, sizeof(ipcam_frame_t));
    s_frame_queue_file = xQueueCreate(IPCAM_FRAME_QUEUE_DEPTH, sizeof(ipcam_frame_t));
    s_snapshot_sem     = xSemaphoreCreateBinary();

    if (s_frame_queue_net == NULL || s_frame_queue_file == NULL || s_snapshot_sem == NULL) {
        LOG_E(TAG, "Queue/semaphore creation failed");
        LED_ERROR_ON();
        for (;;) { __asm("NOP"); }
    }

    /* 9. 创建 FreeRTOS 任务 */
    BaseType_t task_ret;

    task_ret = xTaskCreate(sys_watchdog_task, "WDG",
                           STACK_WATCHDOG, NULL, PRIO_WATCHDOG, NULL);
    configASSERT(task_ret == pdPASS);

    task_ret = xTaskCreate(task_cam_capture, "CAM",
                           STACK_CAM_CAPTURE, NULL, PRIO_CAM_CAPTURE, NULL);
    configASSERT(task_ret == pdPASS);

    task_ret = xTaskCreate(task_net_send, "NET",
                           STACK_NET_SEND, NULL, PRIO_NET_SEND, NULL);
    configASSERT(task_ret == pdPASS);

    task_ret = xTaskCreate(task_file_write, "FILE",
                           STACK_FILE_WRITE, NULL, PRIO_FILE_WRITE, NULL);
    configASSERT(task_ret == pdPASS);

    task_ret = xTaskCreate(task_cmd_handler, "CMD",
                           STACK_CMD_HANDLER, NULL, PRIO_CMD_HANDLER, NULL);
    configASSERT(task_ret == pdPASS);

    task_ret = xTaskCreate(sys_manager_task, "SYS",
                           STACK_SYS_MANAGER, NULL, PRIO_SYS_MANAGER, NULL);
    configASSERT(task_ret == pdPASS);

    /* 启动 WiFi 连接（在调度器启动后由 net_tick 驱动，此处触发首次连接） */
    net_connect();

    LOG_I(TAG, "All tasks created, starting scheduler");

    /* 10. 启动 FreeRTOS 调度器（不返回） */
    vTaskStartScheduler();

    /* 不应到达此处 */
    LOG_E(TAG, "Scheduler returned! System halted.");
    LED_ERROR_ON();
    for (;;) { __asm("NOP"); }

    return 0;
}

/* -----------------------------------------------------------------------
 * FreeRTOS 钩子函数
 * ----------------------------------------------------------------------- */
void vApplicationMallocFailedHook(void)
{
    LOG_E(TAG, "Malloc failed!");
    sys_soft_reset(RESET_REASON_SOFT);
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName)
{
    (void)xTask;
    LOG_E(TAG, "Stack overflow in task: %s", pcTaskName);
    sys_soft_reset(RESET_REASON_SOFT);
}

void vApplicationIdleHook(void)
{
    /* 空闲时进入 Wait For Interrupt 降低功耗 */
    __asm("WFI");
}
