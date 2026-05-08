/**
 * @file file_mgr.c
 * @brief SD 卡文件管理器实现（基于 FatFs R0.15）
 *
 * 录像文件格式（MJPEG 容器）：
 *   每帧 = [4B 小端 frame_size][JPEG data]
 *   文件名：REC_YYYYMMDD_HHMMSS.mjpeg
 *   照片名：SNAP_YYYYMMDD_HHMMSS.jpg
 *
 * 线程安全：所有公开函数均通过 FatFs 内置互斥锁（FF_FS_REENTRANT=1）保护。
 * 调用方需确保不在 ISR 中调用本模块函数。
 */

#include "file_mgr.h"
#include "log.h"
#include "ipcam_config.h"
#include "rtc_driver.h"
#include "ff.h"
#include "fsl_sdhc.h"
#include "fsl_clock.h"
#include "fsl_gpio.h"
#include "board.h"
#include "FreeRTOS.h"
#include "task.h"
#include "semphr.h"
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#define TAG "FM"

/* -----------------------------------------------------------------------
 * 内部状态
 * ----------------------------------------------------------------------- */

/* 写入速率监控 */
typedef struct {
    uint32_t last_check_ms;
    uint64_t last_check_size;
    uint32_t slow_write_secs;
} write_perf_t;

#define WRITE_RATE_MIN_BPS             (4U * 1024U * 1024U)  /**< 4MB/s */
#define WRITE_RATE_CHECK_INTERVAL_MS   1000U
#define WRITE_RATE_SLOW_THRESHOLD_SECS 5U

typedef struct {
    bool     initialized;
    bool     sd_available;
    bool     recording;
    FIL      rec_file;
    char     rec_filename[FM_FILENAME_MAX_LEN];
    uint64_t rec_file_size;
    uint32_t rec_seq;
    FATFS    fs;
    SemaphoreHandle_t mutex;
    write_perf_t perf;
} fm_state_t;

static fm_state_t s_fm;

/* -----------------------------------------------------------------------
 * 内部辅助：获取当前时间字符串（YYYYMMDD_HHMMSS）
 * 使用 RTC 驱动获取真实时间，RTC 不可用时回退到 tick 估算
 * ----------------------------------------------------------------------- */
static void get_timestamp_str(char *buf, size_t len)
{
    rtc_datetime_t dt;
    if (rtc_is_valid() && rtc_get_datetime(&dt) == IPCAM_OK) {
        /* 使用真实 RTC 时间 */
        char ts[16];
        rtc_format_filename(&dt, ts);
        snprintf(buf, len, "%s", ts);
    } else {
        /* RTC 不可用：用 tick 计数生成伪时间戳（格式保持一致） */
        uint32_t tick_sec = (uint32_t)(xTaskGetTickCount() / configTICK_RATE_HZ);
        uint32_t hh = (tick_sec / 3600U) % 24U;
        uint32_t mm = (tick_sec / 60U)   % 60U;
        uint32_t ss =  tick_sec          % 60U;
        snprintf(buf, len, "20260101_%02lu%02lu%02lu",
                 (unsigned long)hh,
                 (unsigned long)mm,
                 (unsigned long)ss);
    }
}

/* -----------------------------------------------------------------------
 * 内部辅助：从文件名解析 Unix 时间戳（简化版，仅解析时分秒）
 * 文件名格式：REC_YYYYMMDD_HHMMSS.mjpeg 或 SNAP_YYYYMMDD_HHMMSS.jpg
 * ----------------------------------------------------------------------- */
static uint32_t parse_timestamp_from_name(const char *filename)
{
    /* 跳过前缀 "REC_" 或 "SNAP_"，找到 YYYYMMDD_HHMMSS 部分 */
    const char *p = strchr(filename, '_');
    if (p == NULL) return 0U;
    p++; /* 跳过 '_' */

    /* 解析 YYYYMMDD */
    char date[9] = {0};
    strncpy(date, p, 8U);
    uint32_t year  = (uint32_t)strtol(date,     NULL, 10);
    uint32_t month = (uint32_t)strtol(date + 4, NULL, 10);
    uint32_t day   = (uint32_t)strtol(date + 6, NULL, 10);
    (void)year; (void)month; (void)day; /* 简化：不做完整日期转换 */

    /* 解析 HHMMSS */
    p += 9U; /* 跳过 "YYYYMMDD_" */
    if (strlen(p) < 6U) return 0U;
    uint32_t hh = (uint32_t)strtol(p,     NULL, 10) / 10000U % 100U;
    uint32_t mm = (uint32_t)strtol(p + 2, NULL, 10) / 100U   % 100U;
    uint32_t ss = (uint32_t)strtol(p + 4, NULL, 10)           % 100U;
    return hh * 3600U + mm * 60U + ss;
}

/* -----------------------------------------------------------------------
 * SDHC 底层初始化（供 FatFs diskio 调用）
 * 此处仅做 SDHC 控制器初始化，diskio.c 中的 disk_initialize 会调用它
 * ----------------------------------------------------------------------- */
static bool sdhc_hw_init(void)
{
    /* 检测 SD 卡插入（CD 引脚低电平 = 已插入） */
    if (GPIO_PinRead(SD_CD_GPIO, SD_CD_PIN) != 0U) {
        LOG_W(TAG, "SD card not detected (CD pin high)");
        return false;
    }

    sdhc_config_t sdhc_cfg = {
        .cardDetectDat3      = false,
        .endianMode          = kSDHC_EndianModeLittle,
        .dmaMode             = kSDHC_DmaModeAdma2,
        .readWatermarkLevel  = 128U,
        .writeWatermarkLevel = 128U,
    };
    SDHC_Init(SD_SDHC, &sdhc_cfg);
    return true;
}

/* -----------------------------------------------------------------------
 * fm_init
 * ----------------------------------------------------------------------- */
ipcam_status_t fm_init(void)
{
    memset(&s_fm, 0, sizeof(s_fm));

    s_fm.mutex = xSemaphoreCreateMutex();
    if (s_fm.mutex == NULL) {
        LOG_E(TAG, "Failed to create mutex");
        return IPCAM_ERR_NOMEM;
    }

    /* 初始化 SDHC 硬件 */
    if (!sdhc_hw_init()) {
        LOG_W(TAG, "SDHC init failed, SD storage disabled");
        s_fm.sd_available = false;
        s_fm.initialized  = true;  /* 模块已初始化，但 SD 不可用 */
        return IPCAM_ERR_IO;
    }

    /* 挂载 FAT32 文件系统（卷号 "0:"） */
    FRESULT fr = f_mount(&s_fm.fs, "0:", 1U);  /* 1 = 立即挂载 */
    if (fr != FR_OK) {
        LOG_E(TAG, "f_mount failed: FRESULT=%d", (int)fr);
        s_fm.sd_available = false;
        s_fm.initialized  = true;
        return IPCAM_ERR_IO;
    }

    s_fm.sd_available = true;
    s_fm.initialized  = true;

    /* 确保录像目录存在 */
    f_mkdir("0:/recordings");
    f_mkdir("0:/snapshots");

    uint32_t free_mb = 0U;
    fm_get_free_space(&free_mb);
    LOG_I(TAG, "SD card mounted OK, free space: %lu MB", (unsigned long)free_mb);

    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * fm_is_sd_available
 * ----------------------------------------------------------------------- */
bool fm_is_sd_available(void)
{
    return s_fm.sd_available;
}

/* -----------------------------------------------------------------------
 * fm_start_recording
 * ----------------------------------------------------------------------- */
ipcam_status_t fm_start_recording(void)
{
    if (!s_fm.initialized) return IPCAM_ERR_NOT_INIT;
    if (!s_fm.sd_available) return IPCAM_ERR_IO;

    /* 检查剩余空间 */
    uint32_t free_mb = 0U;
    fm_get_free_space(&free_mb);
    if (free_mb < IPCAM_SD_LOW_SPACE_MB) {
        LOG_W(TAG, "SD low space (%lu MB), cannot start recording", (unsigned long)free_mb);
        return IPCAM_ERR_FULL;
    }

    if (xSemaphoreTake(s_fm.mutex, pdMS_TO_TICKS(1000U)) != pdTRUE) {
        return IPCAM_ERR_BUSY;
    }

    /* 若已在录像，先关闭当前文件 */
    if (s_fm.recording) {
        f_close(&s_fm.rec_file);
        s_fm.recording = false;
    }

    /* 生成文件名，同秒内追加序号避免重名 */
    char ts[20];
    get_timestamp_str(ts, sizeof(ts));
    if (s_fm.rec_seq > 0U) {
        snprintf(s_fm.rec_filename, FM_FILENAME_MAX_LEN,
                 "0:/recordings/REC_%s_%02lu.mjpeg", ts, (unsigned long)s_fm.rec_seq);
    } else {
        snprintf(s_fm.rec_filename, FM_FILENAME_MAX_LEN,
                 "0:/recordings/REC_%s.mjpeg", ts);
    }
    s_fm.rec_seq++;

    FRESULT fr = f_open(&s_fm.rec_file, s_fm.rec_filename,
                        FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        LOG_E(TAG, "f_open recording failed: %s (FRESULT=%d)",
              s_fm.rec_filename, (int)fr);
        xSemaphoreGive(s_fm.mutex);
        return IPCAM_ERR_IO;
    }

    s_fm.recording      = true;
    s_fm.rec_file_size  = 0U;

    xSemaphoreGive(s_fm.mutex);

    LOG_I(TAG, "Recording started: %s", s_fm.rec_filename);
    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * fm_stop_recording
 * ----------------------------------------------------------------------- */
void fm_stop_recording(void)
{
    if (!s_fm.recording) return;

    if (xSemaphoreTake(s_fm.mutex, pdMS_TO_TICKS(1000U)) != pdTRUE) {
        LOG_W(TAG, "fm_stop_recording: mutex timeout");
        return;
    }

    /* 写入 4 字节结束标记（size=0） */
    uint8_t end_marker[4] = {0U, 0U, 0U, 0U};
    UINT bw;
    f_write(&s_fm.rec_file, end_marker, sizeof(end_marker), &bw);
    f_sync(&s_fm.rec_file);
    f_close(&s_fm.rec_file);
    s_fm.recording = false;

    xSemaphoreGive(s_fm.mutex);

    LOG_I(TAG, "Recording stopped: %s (%.1f MB)",
          s_fm.rec_filename,
          (double)s_fm.rec_file_size / (1024.0 * 1024.0));
}

/* -----------------------------------------------------------------------
 * fm_write_frame
 * ----------------------------------------------------------------------- */
ipcam_status_t fm_write_frame(const uint8_t *data, uint32_t size)
{
    if (!s_fm.initialized) return IPCAM_ERR_NOT_INIT;
    if (!s_fm.sd_available) return IPCAM_ERR_IO;
    if (!s_fm.recording)    return IPCAM_ERR_NOT_INIT;
    if (data == NULL || size == 0U) return IPCAM_ERR_INVALID;

    if (xSemaphoreTake(s_fm.mutex, pdMS_TO_TICKS(50U)) != pdTRUE) {
        /* 超时不阻塞视频流，直接丢帧 */
        LOG_W(TAG, "fm_write_frame: mutex timeout, frame dropped");
        return IPCAM_ERR_BUSY;
    }

    UINT bw;
    FRESULT fr;

    /* 写入 4 字节小端帧长度头 */
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(size & 0xFFU);
    hdr[1] = (uint8_t)((size >> 8U)  & 0xFFU);
    hdr[2] = (uint8_t)((size >> 16U) & 0xFFU);
    hdr[3] = (uint8_t)((size >> 24U) & 0xFFU);

    fr = f_write(&s_fm.rec_file, hdr, sizeof(hdr), &bw);
    if (fr != FR_OK || bw != sizeof(hdr)) {
        LOG_E(TAG, "fm_write_frame: header write failed (FRESULT=%d, bw=%u)",
              (int)fr, (unsigned)bw);
        xSemaphoreGive(s_fm.mutex);
        return IPCAM_ERR_IO;
    }

    /* 写入 JPEG 数据 */
    fr = f_write(&s_fm.rec_file, data, (UINT)size, &bw);
    if (fr != FR_OK || bw != (UINT)size) {
        LOG_E(TAG, "fm_write_frame: data write failed (FRESULT=%d, bw=%u/%lu)",
              (int)fr, (unsigned)bw, (unsigned long)size);
        xSemaphoreGive(s_fm.mutex);
        return IPCAM_ERR_IO;
    }

    s_fm.rec_file_size += (uint64_t)(sizeof(hdr) + size);

    /* 每 30 帧 sync 一次，平衡性能与数据安全 */
    static uint32_t sync_counter = 0U;
    sync_counter++;
    if (sync_counter >= 30U) {
        f_sync(&s_fm.rec_file);
        sync_counter = 0U;
    }

    /* 写入速率监控（每秒检查一次） */
    uint32_t now_ms = (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
    if ((now_ms - s_fm.perf.last_check_ms) >= WRITE_RATE_CHECK_INTERVAL_MS) {
        uint64_t bytes_written = s_fm.rec_file_size - s_fm.perf.last_check_size;
        uint32_t elapsed_ms    = now_ms - s_fm.perf.last_check_ms;
        uint32_t rate_bps      = (uint32_t)((bytes_written * 1000U) / elapsed_ms);

        if (rate_bps < WRITE_RATE_MIN_BPS) {
            s_fm.perf.slow_write_secs++;
            LOG_W(TAG, "Write rate low: %lu KB/s (threshold %lu KB/s), slow_secs=%lu",
                  (unsigned long)(rate_bps / 1024U),
                  (unsigned long)(WRITE_RATE_MIN_BPS / 1024U),
                  (unsigned long)s_fm.perf.slow_write_secs);
        } else {
            s_fm.perf.slow_write_secs = 0U;
        }

        s_fm.perf.last_check_ms   = now_ms;
        s_fm.perf.last_check_size = s_fm.rec_file_size;
    }

    xSemaphoreGive(s_fm.mutex);
    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * fm_save_snapshot
 * ----------------------------------------------------------------------- */
ipcam_status_t fm_save_snapshot(const uint8_t *data, uint32_t size, char *out_path)
{
    if (!s_fm.initialized) return IPCAM_ERR_NOT_INIT;
    if (!s_fm.sd_available) return IPCAM_ERR_IO;
    if (data == NULL || size == 0U) return IPCAM_ERR_INVALID;

    char ts[20];
    get_timestamp_str(ts, sizeof(ts));

    char filepath[FM_FILENAME_MAX_LEN + 16U];
    snprintf(filepath, sizeof(filepath), "0:/snapshots/SNAP_%s.jpg", ts);

    FIL fil;
    FRESULT fr = f_open(&fil, filepath, FA_WRITE | FA_CREATE_ALWAYS);
    if (fr != FR_OK) {
        LOG_E(TAG, "fm_save_snapshot: f_open failed: %s (FRESULT=%d)",
              filepath, (int)fr);
        return IPCAM_ERR_IO;
    }

    UINT bw;
    fr = f_write(&fil, data, (UINT)size, &bw);
    f_close(&fil);

    if (fr != FR_OK || bw != (UINT)size) {
        LOG_E(TAG, "fm_save_snapshot: write failed (FRESULT=%d, bw=%u/%lu)",
              (int)fr, (unsigned)bw, (unsigned long)size);
        return IPCAM_ERR_IO;
    }

    /* 返回不含卷号的路径（供服务器端使用） */
    if (out_path != NULL) {
        /* 跳过 "0:/" 前缀 */
        const char *rel = filepath + 3U;
        strncpy(out_path, rel, FM_FILENAME_MAX_LEN - 1U);
        out_path[FM_FILENAME_MAX_LEN - 1U] = '\0';
    }

    LOG_I(TAG, "Snapshot saved: %s (%lu bytes)", filepath, (unsigned long)size);
    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * fm_get_free_space
 * ----------------------------------------------------------------------- */
ipcam_status_t fm_get_free_space(uint32_t *free_mb)
{
    if (free_mb == NULL) return IPCAM_ERR_INVALID;
    if (!s_fm.sd_available) {
        *free_mb = 0U;
        return IPCAM_ERR_IO;
    }

    DWORD   free_clust;
    FATFS  *fs_ptr = &s_fm.fs;
    FRESULT fr = f_getfree("0:", &free_clust, &fs_ptr);
    if (fr != FR_OK) {
        LOG_W(TAG, "f_getfree failed: FRESULT=%d", (int)fr);
        *free_mb = 0U;
        return IPCAM_ERR_IO;
    }

    /* 计算剩余字节数：free_clust * sectors_per_cluster * 512 */
    uint64_t free_bytes = (uint64_t)free_clust
                        * (uint64_t)fs_ptr->csize
                        * 512ULL;
    *free_mb = (uint32_t)(free_bytes / (1024ULL * 1024ULL));
    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * fm_list_recordings
 * ----------------------------------------------------------------------- */
ipcam_status_t fm_list_recordings(fm_file_info_t *list, uint32_t *count, uint32_t max)
{
    if (list == NULL || count == NULL || max == 0U) return IPCAM_ERR_INVALID;
    if (!s_fm.sd_available) return IPCAM_ERR_IO;

    *count = 0U;

    DIR     dir;
    FILINFO fno;
    FRESULT fr = f_opendir(&dir, "0:/recordings");
    if (fr != FR_OK) {
        LOG_W(TAG, "fm_list_recordings: f_opendir failed: FRESULT=%d", (int)fr);
        return IPCAM_ERR_IO;
    }

    while (*count < max) {
        fr = f_readdir(&dir, &fno);
        if (fr != FR_OK || fno.fname[0] == '\0') {
            break;  /* 目录读完或出错 */
        }
        /* 跳过目录和非 .mjpeg 文件 */
        if (fno.fattrib & AM_DIR) continue;
        if (strstr(fno.fname, ".mjpeg") == NULL &&
            strstr(fno.fname, ".MJPEG") == NULL) continue;

        fm_file_info_t *entry = &list[*count];
        strncpy(entry->filename, fno.fname, FM_FILENAME_MAX_LEN - 1U);
        entry->filename[FM_FILENAME_MAX_LEN - 1U] = '\0';
        entry->size_bytes  = (uint32_t)fno.fsize;
        entry->timestamp   = parse_timestamp_from_name(fno.fname);
        (*count)++;
    }

    f_closedir(&dir);
    LOG_D(TAG, "fm_list_recordings: found %lu files", (unsigned long)*count);
    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * fm_check_rotate
 * ----------------------------------------------------------------------- */
ipcam_status_t fm_check_rotate(void)
{
    if (!s_fm.recording) return IPCAM_OK;

    if (s_fm.rec_file_size >= IPCAM_FILE_MAX_SIZE_GB) {
        LOG_I(TAG, "Recording file reached %.1f GB, rotating",
              (double)s_fm.rec_file_size / (1024.0 * 1024.0 * 1024.0));
        fm_stop_recording();
        return fm_start_recording();
    }
    return IPCAM_OK;
}

/* -----------------------------------------------------------------------
 * fm_get_current_file_size
 * ----------------------------------------------------------------------- */
uint64_t fm_get_current_file_size(void)
{
    return s_fm.rec_file_size;
}

uint32_t fm_get_slow_write_secs(void)
{
    return s_fm.perf.slow_write_secs;
}
