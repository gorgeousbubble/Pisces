/*---------------------------------------------------------------------------/
/  FatFs - Configurable Module Configuration File  R0.15
/---------------------------------------------------------------------------/
/ CAUTION! Do NOT modify this file directly. Copy it to your project and
/ modify the copy. The original file is provided as a reference.
/
/ Pisces IP Camera 项目配置版本
/---------------------------------------------------------------------------*/

#ifndef FFCONF_DEF
#define FFCONF_DEF 80286  /* Revision ID */

/*---------------------------------------------------------------------------/
/ Function Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_READONLY  0   /* 0:Read/Write or 1:Read-only */
#define FF_FS_MINIMIZE  0   /* 0:Full, 1:No f_stat/f_getfree/f_truncate/f_opendir/f_readdir/f_closedir */

#define FF_USE_STRFUNC  1   /* 0:Disable or 1:Enable string functions */
#define FF_USE_FIND     0   /* 0:Disable or 1:Enable f_findfirst/f_findnext */
#define FF_USE_MKFS     1   /* 0:Disable or 1:Enable f_mkfs */
#define FF_USE_FASTSEEK 0   /* 0:Disable or 1:Enable fast seek feature */
#define FF_USE_EXPAND   0   /* 0:Disable or 1:Enable f_expand */
#define FF_USE_CHMOD    0   /* 0:Disable or 1:Enable f_chmod/f_utime */
#define FF_USE_LABEL    0   /* 0:Disable or 1:Enable f_getlabel/f_setlabel */
#define FF_USE_FORWARD  0   /* 0:Disable or 1:Enable f_forward */
#define FF_USE_MKFS     1

/*---------------------------------------------------------------------------/
/ Locale and Namespace Configurations
/---------------------------------------------------------------------------*/

#define FF_CODE_PAGE    936  /* 936 = Simplified Chinese GBK */

#define FF_USE_LFN      1   /* 0:Disable LFN, 1:Enable LFN with static buffer */
#define FF_MAX_LFN      255
#define FF_LFN_UNICODE  0   /* 0:ANSI/OEM, 1:UTF-16, 2:UTF-8, 3:UTF-32 */
#define FF_LFN_BUF      255
#define FF_SFN_BUF      12

#define FF_FS_RPATH     0   /* 0:Disable relative path, 1:Enable */

/*---------------------------------------------------------------------------/
/ Drive/Volume Configurations
/---------------------------------------------------------------------------*/

#define FF_VOLUMES      1   /* Number of volumes (logical drives) */
#define FF_STR_VOLUME_ID 0
#define FF_VOLUME_STRS  "SD"

#define FF_MULTI_PARTITION 0

#define FF_MIN_SS       512
#define FF_MAX_SS       512  /* 固定 512 字节扇区，与 SD 卡一致 */

#define FF_LBA64        0
#define FF_FS_EXFAT     0

#define FF_FS_NORTC     0   /* 0:Use RTC (get_fattime), 1:Use fixed time */
#define FF_NORTC_MON    1
#define FF_NORTC_MDAY   1
#define FF_NORTC_YEAR   2026

#define FF_FS_NOFSINFO  0

/*---------------------------------------------------------------------------/
/ System Configurations
/---------------------------------------------------------------------------*/

#define FF_FS_TINY      0   /* 0:Normal or 1:Tiny (reduces code size) */
#define FF_FS_EXFAT     0

#define FF_FS_LOCK      4   /* 0:Disable or >=1:Enable with N open objects */

/*
 * 重入保护（线程安全）
 * FF_FS_REENTRANT = 1 时，FatFs 使用 ffsystem.c 中的互斥锁接口
 * FF_SYNC_t 必须与 ffsystem.c 中使用的类型一致（FreeRTOS SemaphoreHandle_t）
 * FF_FS_TIMEOUT 单位为 FreeRTOS tick（configTICK_RATE_HZ=1000 时，1000=1秒）
 */
#define FF_FS_REENTRANT 1
#define FF_FS_TIMEOUT   1000   /* 互斥锁等待超时：1000 tick = 1 秒 */
#define FF_SYNC_t       SemaphoreHandle_t

#endif /* FFCONF_DEF */
