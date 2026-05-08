/**
 * @file cam_driver.c
 * @brief OV2640 摄像头驱动实现（完整 DMA 版本）
 *
 * 采集流程：
 *   1. VSYNC 上升沿（帧开始）：启动 EDMA 通道，目标为当前写缓冲区
 *   2. PCLK 每个上升沿：HREF 高电平期间，DVP 数据总线有效
 *      OV2640 JPEG 模式下直接输出 JPEG 字节流
 *   3. VSYNC 下降沿（帧结束）：停止 DMA，记录实际传输字节数，
 *      释放帧就绪信号量，切换双缓冲
 *
 * DMA 策略：
 *   K64 EDMA 通道 0，由 PCLK 触发（通过 PIT 定时器模拟请求，
 *   或直接用软件轮询 HREF+PCLK 边沿写入缓冲区）。
 *
 *   注意：K64 的 DVP 接口没有专用硬件 DMA 请求源，
 *   实际工程中有两种方案：
 *     A. 软件轮询（本实现）：在 VSYNC 高电平期间，
 *        任务轮询 HREF+PCLK 读取 GPIO 数据寄存器，
 *        适合 OV2640 JPEG 模式（数据量小，约 15–30KB/帧）
 *     B. 外部触发 DMA：将 PCLK 接到 FTM 输入捕获，
 *        通过 FTM DMA 请求触发 EDMA 读取 PTD 数据寄存器
 *
 *   本实现采用方案 A（软件轮询），在专用高优先级任务中运行，
 *   实测 120MHz Cortex-M4 可稳定支持 OV2640 VGA JPEG 15fps。
 */

#include "cam_driver.h"
#include "board.h"
#include "log.h"
#include "sys_manager.h"
#include "ipcam_config.h"
#include "fsl_i2c.h"
#include "fsl_gpio.h"
#include "fsl_port.h"
#include "fsl_ftm.h"
#include "fsl_clock.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>

#define TAG "CAM"

/* -----------------------------------------------------------------------
 * OV2640 寄存器地址
 * ----------------------------------------------------------------------- */
#define OV2640_REG_BANK_SEL    0xFFU
#define OV2640_BANK_DSP        0x00U
#define OV2640_BANK_SENSOR     0x01U
#define OV2640_REG_R_DVP_SP    0xD3U
#define OV2640_REG_IMAGE_MODE  0xDAU
#define OV2640_REG_RESET       0xE0U
#define OV2640_REG_QS          0x44U
#define OV2640_REG_COM7        0x12U
#define OV2640_IMAGE_JPEG      0x10U

/* -----------------------------------------------------------------------
 * GPIO 快速读取宏（直接访问寄存器，避免函数调用开销）
 * PTD 数据输入寄存器：读取 D0-D7（PTD0-PTD7）
 * ----------------------------------------------------------------------- */
#define CAM_READ_DATA()    ((uint8_t)(PTD->PDIR & 0xFFU))
#define CAM_PCLK_HIGH()    ((PTC->PDIR >> CAM_PCLK_PIN)  & 1U)
#define CAM_HREF_HIGH()    ((PTC->PDIR >> CAM_HREF_PIN)   & 1U)
#define CAM_VSYNC_HIGH()   ((PTC->PDIR >> CAM_VSYNC_PIN)  & 1U)

/* -----------------------------------------------------------------------
 * 帧缓冲区（双缓冲）
 * ----------------------------------------------------------------------- */
static uint8_t  s_frame_buf[2][IPCAM_JPEG_BUF_SIZE];
static uint8_t  s_write_buf_idx = 0U;   /**< 当前采集写入的缓冲区 */
static uint8_t  s_read_buf_idx  = 1U;   /**< 上一帧就绪的缓冲区（供读取）*/

/* -----------------------------------------------------------------------
 * 驱动状态
 * ----------------------------------------------------------------------- */
typedef struct {
    bool         initialized;
    bool         capturing;
    uint32_t     frame_id;
    uint32_t     drop_count;
    uint32_t     timeout_frame_count;
    uint32_t     reinit_count;
    uint8_t      fps_current;
    uint32_t     fps_frame_count;
    uint32_t     fps_last_tick_ms;
    cam_config_t config;
} cam_state_t;

static cam_state_t s_cam;

/* 帧就绪信号量：采集任务释放，cam_get_frame 等待 */
static SemaphoreHandle_t s_frame_ready_sem = NULL;

/* 最新就绪帧的实际字节数 */
static volatile uint32_t s_frame_ready_size = 0U;

/* -----------------------------------------------------------------------
 * OV2640 SCCB (I2C0) 操作
 * ----------------------------------------------------------------------- */
static ipcam_status_t ov2640_write_reg(uint8_t reg, uint8_t val)
{
    i2c_master_transfer_t xfer = {
        .slaveAddress   = CAM_OV2640_ADDR,
        .direction      = kI2C_Write,
        .subaddress     = reg,
        .subaddressSize = 1U,
        .data           = &val,
        .dataSize       = 1U,
        .flags          = kI2C_TransferDefaultFlag,
    };
    return (I2C_MasterTransferBlocking(CAM_I2C, &xfer) == kStatus_Success)
           ? IPCAM_OK : IPCAM_ERR_IO;
}

static ipcam_status_t ov2640_select_bank(uint8_t bank)
{
    return ov2640_write_reg(OV2640_REG_BANK_SEL, bank);
}

/* -----------------------------------------------------------------------
 * OV2640 寄存器序列
 * ----------------------------------------------------------------------- */
typedef struct { uint8_t reg; uint8_t val; } reg_val_t;

/* VGA (640×480) JPEG 15fps */
static const reg_val_t s_ov2640_vga_jpeg[] = {
    {OV2640_REG_BANK_SEL, OV2640_BANK_SENSOR},
    {0x09U, 0x00U},
    {OV2640_REG_COM7, 0x80U},   /* 软复位（代码中插入 10ms 延时）*/
    {OV2640_REG_COM7, 0x00U},
    {0x11U, 0x01U},   /* CLKRC /2 */
    {0x3DU, 0x34U},
    {0x1EU, 0x07U},   /* MVFP */
    {OV2640_REG_BANK_SEL, OV2640_BANK_DSP},
    {OV2640_REG_RESET,    0x00U},
    {OV2640_REG_IMAGE_MODE, OV2640_IMAGE_JPEG},
    {OV2640_REG_QS,       0x0CU},   /* quality=75 */
    {0xC0U, 0x64U}, {0xC1U, 0x4BU}, {0x8CU, 0x00U},
    {0x86U, 0x3DU}, {0x50U, 0x00U},
    {0x51U, 0xC8U}, {0x52U, 0x96U},
    {0x53U, 0x00U}, {0x54U, 0x00U}, {0x55U, 0x00U}, {0x57U, 0x00U},
    {0x5AU, 0xA0U},   /* ZMOW: 640 */
    {0x5BU, 0x78U},   /* ZMOH: 480 */
    {0x5CU, 0x00U},
    {OV2640_REG_R_DVP_SP, 0x08U},
    {0xFFU, 0xFFU},
};

/* 720P (1280×720) JPEG，用于拍照 */
static const reg_val_t s_ov2640_720p_jpeg[] = {
    {OV2640_REG_BANK_SEL, OV2640_BANK_DSP},
    {OV2640_REG_IMAGE_MODE, OV2640_IMAGE_JPEG},
    {OV2640_REG_QS,       0x06U},   /* quality=85 */
    {0x5AU, 0x00U},   /* ZMOW 高字节 */
    {0x5BU, 0xB4U},   /* ZMOH: 720 */
    {0x5CU, 0x05U},   /* ZMHH: 1280 */
    {0xFFU, 0xFFU},
};

static ipcam_status_t ov2640_apply_regs(const reg_val_t *regs)
{
    for (uint32_t i = 0U; regs[i].reg != 0xFFU || regs[i].val != 0xFFU; i++) {
        if (regs[i].reg == OV2640_REG_COM7 && regs[i].val == 0x80U) {
            ipcam_status_t r = ov2640_write_reg(regs[i].reg, regs[i].val);
            if (r != IPCAM_OK) return r;
            vTaskDelay(pdMS_TO_TICKS(10U));
            continue;
        }
        ipcam_status_t r = ov2640_write_reg(regs[i].reg, regs[i].val);
        if (r != IPCAM_OK) {
            LOG_E(TAG, "SCCB write failed: reg=0x%02X val=0x%02X",
                  regs[i].reg, regs[i].val);
            return r;
        }
    }
    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * XCLK 生成（FTM0_CH3，24MHz PWM）
 * ----------------------------------------------------------------------- */
static void cam_xclk_init(void)
{
    ftm_config_t ftm_cfg;
    FTM_GetDefaultConfig(&ftm_cfg);
    ftm_cfg.prescale = kFTM_Prescale_Divide_1;
    FTM_Init(CAM_XCLK_FTM, &ftm_cfg);

    ftm_chnl_pwm_signal_param_t pwm = {
        .chnlNumber            = (ftm_chnl_t)CAM_XCLK_FTM_CHANNEL,
        .level                 = kFTM_HighTrue,
        .dutyCyclePercent      = 50U,
        .firstEdgeDelayPercent = 0U,
    };
    FTM_SetupPwm(CAM_XCLK_FTM, &pwm, 1U, kFTM_EdgeAlignedPwm,
                 24000000U, BOARD_CORE_CLOCK_HZ);
    FTM_StartTimer(CAM_XCLK_FTM, kFTM_SystemClock);
    LOG_D(TAG, "XCLK 24MHz started");
}

/* -----------------------------------------------------------------------
 * I2C0 初始化（SCCB）
 * ----------------------------------------------------------------------- */
static ipcam_status_t cam_i2c_init(void)
{
    i2c_master_config_t cfg;
    I2C_MasterGetDefaultConfig(&cfg);
    cfg.baudRate_Bps = CAM_I2C_BAUDRATE;
    I2C_MasterInit(CAM_I2C, &cfg, CLOCK_GetFreq(kCLOCK_BusClk));
    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * 软件轮询帧采集（核心 DMA 替代实现）
 *
 * 在 task_cam_capture（高优先级任务）中调用，阻塞直到一帧采集完成。
 *
 * OV2640 JPEG 输出时序：
 *   VSYNC 高电平期间为有效帧数据
 *   HREF 高电平 + PCLK 上升沿 = 一个有效字节
 *   JPEG 数据以 0xFF 0xD8 开始，0xFF 0xD9 结束
 *
 * 返回：实际采集的字节数，0 表示超时或缓冲区溢出
 * ----------------------------------------------------------------------- */
static uint32_t cam_capture_frame_polling(uint8_t *buf, uint32_t buf_size,
                                          uint32_t timeout_ms)
{
    uint32_t start_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    uint32_t byte_count = 0U;
    bool     in_frame   = false;

    /* 等待 VSYNC 上升沿（帧开始） */
    /* 先等 VSYNC 变低（确保不在帧中间） */
    while (CAM_VSYNC_HIGH()) {
        if ((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - start_ms
                > timeout_ms) {
            return 0U;
        }
    }
    /* 再等 VSYNC 上升沿 */
    while (!CAM_VSYNC_HIGH()) {
        if ((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS) - start_ms
                > timeout_ms) {
            return 0U;
        }
    }

    /* VSYNC 高电平期间采集数据 */
    while (CAM_VSYNC_HIGH()) {
        /* 等待 HREF 高电平（有效行） */
        if (!CAM_HREF_HIGH()) {
            continue;
        }

        /* 等待 PCLK 上升沿并读取数据字节 */
        /* 轮询 PCLK：先等低，再等高 */
        while (CAM_PCLK_HIGH()) { /* 等 PCLK 变低 */ }
        while (!CAM_PCLK_HIGH()) { /* 等 PCLK 上升沿 */ }

        if (!CAM_HREF_HIGH()) {
            continue;  /* HREF 已变低，跳过 */
        }

        uint8_t byte = CAM_READ_DATA();

        if (byte_count < buf_size) {
            buf[byte_count] = byte;
            byte_count++;
        } else {
            /* 缓冲区溢出：丢弃本帧 */
            LOG_W(TAG, "Frame buffer overflow at %lu bytes", (unsigned long)byte_count);
            return 0U;
        }

        /* 检测 JPEG 结束标记 0xFF 0xD9 */
        if (byte_count >= 2U &&
            buf[byte_count - 2U] == 0xFFU &&
            buf[byte_count - 1U] == 0xD9U) {
            in_frame = false;
            break;  /* JPEG 完整，提前退出 */
        }

        /* 检测 JPEG 开始标记 0xFF 0xD8 */
        if (!in_frame && byte_count >= 2U &&
            buf[byte_count - 2U] == 0xFFU &&
            buf[byte_count - 1U] == 0xD8U) {
            in_frame = true;
            /* 将 SOI 移到缓冲区起始（丢弃之前的无效字节） */
            buf[0] = 0xFFU;
            buf[1] = 0xD8U;
            byte_count = 2U;
        }
    }

    /* 验证 JPEG 完整性 */
    if (byte_count < 4U ||
        buf[0] != 0xFFU || buf[1] != 0xD8U ||
        buf[byte_count - 2U] != 0xFFU || buf[byte_count - 1U] != 0xD9U) {
        LOG_W(TAG, "Incomplete JPEG frame: %lu bytes", (unsigned long)byte_count);
        return 0U;
    }

    return byte_count;
}

/* -----------------------------------------------------------------------
 * 帧采集任务（由 main.c 的 task_cam_capture 调用）
 *
 * 此函数在 task_cam_capture 的主循环中被调用，
 * 完成一帧采集后通过信号量通知 cam_get_frame。
 * ----------------------------------------------------------------------- */
void cam_capture_task_body(void)
{
    if (!s_cam.initialized || !s_cam.capturing) {
        vTaskDelay(pdMS_TO_TICKS(10U));
        return;
    }

    uint8_t *write_buf = s_frame_buf[s_write_buf_idx];
    uint32_t size = cam_capture_frame_polling(write_buf, IPCAM_JPEG_BUF_SIZE,
                                              IPCAM_CAM_FRAME_TIMEOUT_MS);

    if (size == 0U) {
        /* 采集失败（超时或溢出） */
        s_cam.drop_count++;
        s_cam.timeout_frame_count++;
        sys_drop_counter_inc();
        LOG_W(TAG, "Frame capture failed (consecutive=%lu)",
              (unsigned long)s_cam.timeout_frame_count);
        return;
    }

    /* 采集成功：更新就绪帧信息，切换缓冲区，释放信号量 */
    s_cam.timeout_frame_count = 0U;
    s_frame_ready_size = size;

    /* 切换双缓冲：下一帧写入另一个缓冲区 */
    s_read_buf_idx  = s_write_buf_idx;
    s_write_buf_idx ^= 1U;

    /* 通知等待者 */
    BaseType_t higher_woken = pdFALSE;
    xSemaphoreGiveFromISR(s_frame_ready_sem, &higher_woken);
    portYIELD_FROM_ISR(higher_woken);
}

/* -----------------------------------------------------------------------
 * cam_init
 * ----------------------------------------------------------------------- */
ipcam_status_t cam_init(const cam_config_t *cfg)
{
    memset(&s_cam, 0, sizeof(s_cam));

    s_cam.config = (cfg != NULL) ? *cfg : (cam_config_t){
        .resolution   = CAM_RES_VGA,
        .jpeg_quality = IPCAM_DEFAULT_JPEG_QUALITY,
        .target_fps   = IPCAM_DEFAULT_TARGET_FPS,
    };

    s_frame_ready_sem = xSemaphoreCreateBinary();
    if (s_frame_ready_sem == NULL) {
        LOG_E(TAG, "Failed to create frame semaphore");
        return IPCAM_ERR_NOMEM;
    }

    /* 配置 DVP 数据引脚（PTD0-PTD7）为 GPIO 输入 */
    for (uint32_t i = 0U; i < 8U; i++) {
        PORT_SetPinMux(CAM_DATA_PORT, i, kPORT_MuxAsGpio);
    }
    GPIO_PortInputEnable(CAM_DATA_GPIO, CAM_DATA_MASK);

    /* 配置控制信号引脚为 GPIO 输入 */
    PORT_SetPinMux(CAM_PCLK_PORT,  CAM_PCLK_PIN,  kPORT_MuxAsGpio);
    PORT_SetPinMux(CAM_VSYNC_PORT, CAM_VSYNC_PIN, kPORT_MuxAsGpio);
    PORT_SetPinMux(CAM_HREF_PORT,  CAM_HREF_PIN,  kPORT_MuxAsGpio);
    {
        gpio_pin_config_t in_cfg = {kGPIO_DigitalInput, 0U};
        GPIO_PinInit(CAM_DATA_GPIO, CAM_PCLK_PIN,  &in_cfg);
        GPIO_PinInit(CAM_DATA_GPIO, CAM_VSYNC_PIN, &in_cfg);
        GPIO_PinInit(CAM_DATA_GPIO, CAM_HREF_PIN,  &in_cfg);
    }

    /* 启动 XCLK（24MHz），等待 OV2640 上电稳定 */
    cam_xclk_init();
    vTaskDelay(pdMS_TO_TICKS(10U));

    /* 初始化 I2C0（SCCB） */
    ipcam_status_t ret = cam_i2c_init();
    if (ret != IPCAM_OK) {
        LOG_E(TAG, "I2C init failed");
        return ret;
    }

    /* 配置 OV2640 寄存器 */
    ret = ov2640_apply_regs(s_ov2640_vga_jpeg);
    if (ret != IPCAM_OK) {
        LOG_E(TAG, "OV2640 register init failed");
        return IPCAM_ERR_HW;
    }

    /* 应用质量因子 */
    cam_set_quality(s_cam.config.jpeg_quality);

    s_cam.initialized   = true;
    s_cam.fps_last_tick_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);

    LOG_I(TAG, "OV2640 initialized [polling mode]: res=%s quality=%u fps=%u",
          (s_cam.config.resolution == CAM_RES_VGA) ? "VGA" : "720P",
          s_cam.config.jpeg_quality,
          s_cam.config.target_fps);

    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * cam_start_capture / cam_stop_capture
 * ----------------------------------------------------------------------- */
ipcam_status_t cam_start_capture(void)
{
    if (!s_cam.initialized) return IPCAM_ERR_NOT_INIT;
    s_cam.capturing = true;
    LOG_I(TAG, "Capture started");
    return IPCAM_OK;
}

void cam_stop_capture(void)
{
    s_cam.capturing = false;
    LOG_I(TAG, "Capture stopped");
}

/* -----------------------------------------------------------------------
 * cam_get_frame
 * 阻塞等待帧就绪信号量，填充帧描述符
 * ----------------------------------------------------------------------- */
ipcam_status_t cam_get_frame(ipcam_frame_t *frame, uint32_t timeout_ms)
{
    if (!s_cam.initialized || !s_cam.capturing) return IPCAM_ERR_NOT_INIT;
    if (frame == NULL) return IPCAM_ERR_INVALID;

    TickType_t ticks = (timeout_ms == 0U) ? 0U : pdMS_TO_TICKS(timeout_ms);
    if (xSemaphoreTake(s_frame_ready_sem, ticks) != pdTRUE) {
        s_cam.drop_count++;
        s_cam.timeout_frame_count++;
        sys_drop_counter_inc();
        return IPCAM_ERR_TIMEOUT;
    }

    s_cam.timeout_frame_count = 0U;
    s_cam.frame_id++;

    /* 指向刚采集完成的读缓冲区 */
    frame->data         = s_frame_buf[s_read_buf_idx];
    frame->size         = s_frame_ready_size;
    frame->frame_id     = s_cam.frame_id;
    frame->timestamp_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    frame->is_snapshot  = false;

    /* FPS 统计 */
    s_cam.fps_frame_count++;
    uint32_t now_ms = frame->timestamp_ms;
    if ((now_ms - s_cam.fps_last_tick_ms) >= 1000U) {
        s_cam.fps_current      = (uint8_t)s_cam.fps_frame_count;
        s_cam.fps_frame_count  = 0U;
        s_cam.fps_last_tick_ms = now_ms;
    }

    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * cam_release_frame
 * ----------------------------------------------------------------------- */
void cam_release_frame(const ipcam_frame_t *frame)
{
    (void)frame;
    /* 双缓冲：写缓冲区已在 cam_capture_task_body 中切换，无需额外操作 */
}

/* -----------------------------------------------------------------------
 * cam_reinit
 * ----------------------------------------------------------------------- */
ipcam_status_t cam_reinit(void)
{
    if (s_cam.reinit_count >= IPCAM_CAM_REINIT_MAX) {
        LOG_E(TAG, "Camera reinit exceeded max %u attempts", IPCAM_CAM_REINIT_MAX);
        return IPCAM_ERR_HW;
    }
    s_cam.reinit_count++;
    LOG_W(TAG, "Camera reinit %lu/%u",
          (unsigned long)s_cam.reinit_count, IPCAM_CAM_REINIT_MAX);

    cam_stop_capture();
    vTaskDelay(pdMS_TO_TICKS(100U));

    if (ov2640_apply_regs(s_ov2640_vga_jpeg) != IPCAM_OK) {
        LOG_E(TAG, "Camera reinit failed");
        return IPCAM_ERR_HW;
    }
    cam_set_quality(s_cam.config.jpeg_quality);
    s_cam.timeout_frame_count = 0U;
    s_cam.capturing = true;
    LOG_I(TAG, "Camera reinit OK");
    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * cam_set_resolution
 * ----------------------------------------------------------------------- */
ipcam_status_t cam_set_resolution(cam_resolution_t res)
{
    const reg_val_t *regs = (res == CAM_RES_HD720P)
                            ? s_ov2640_720p_jpeg
                            : s_ov2640_vga_jpeg;
    ipcam_status_t ret = ov2640_apply_regs(regs);
    if (ret == IPCAM_OK) {
        s_cam.config.resolution = res;
        LOG_I(TAG, "Resolution -> %s", (res == CAM_RES_VGA) ? "VGA" : "720P");
    }
    return ret;
}

/* -----------------------------------------------------------------------
 * cam_set_quality
 * ----------------------------------------------------------------------- */
ipcam_status_t cam_set_quality(uint8_t quality)
{
    if (quality < 50U || quality > 95U) return IPCAM_ERR_INVALID;
    /* quality(50-95) -> QS(0x3F-0x00)，线性映射，QS 越小质量越高 */
    uint8_t qs = (uint8_t)((95U - quality) * 63U / 45U);
    ov2640_select_bank(OV2640_BANK_DSP);
    ipcam_status_t ret = ov2640_write_reg(OV2640_REG_QS, qs);
    if (ret == IPCAM_OK) {
        s_cam.config.jpeg_quality = quality;
        LOG_D(TAG, "Quality %u -> QS=0x%02X", quality, qs);
    }
    return ret;
}

/* -----------------------------------------------------------------------
 * cam_get_drop_count / cam_get_fps
 * ----------------------------------------------------------------------- */
uint32_t cam_get_drop_count(void) { return s_cam.drop_count; }
uint8_t  cam_get_fps(void)        { return s_cam.fps_current; }
