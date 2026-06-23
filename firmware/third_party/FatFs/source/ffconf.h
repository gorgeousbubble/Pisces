/*---------------------------------------------------------------------------/
/  FatFs - Configuration File  R0.15
/  Pisces IP Camera — SDK_2_11_0_FRDM-K64F + FreeRTOS
/---------------------------------------------------------------------------*/

#ifndef FFCONF_DEF
#define FFCONF_DEF 80286

/*---------------------------------------------------------------------------/
/ Function Configurations
/---------------------------------------------------------------------------*/
#define FF_FS_READONLY   0
#define FF_FS_MINIMIZE   0
#define FF_USE_STRFUNC   1
#define FF_USE_FIND      0
#define FF_USE_MKFS      1   /* 只定义一次 */
#define FF_USE_FASTSEEK  0
#define FF_USE_EXPAND    0
#define FF_USE_CHMOD     0
#define FF_USE_LABEL     0
#define FF_USE_FORWARD   0

/*---------------------------------------------------------------------------/
/ Locale and Namespace Configurations
/---------------------------------------------------------------------------*/
#define FF_CODE_PAGE     936

/*
 * FF_USE_LFN = 2: 长文件名，缓冲区在调用栈上分配（线程安全）
 * 不能用 FF_USE_LFN=1（静态缓冲区）与 FF_FS_REENTRANT=1 同时使用
 */
#define FF_USE_LFN       2
#define FF_MAX_LFN       255
#define FF_LFN_UNICODE   0
#define FF_LFN_BUF       255
#define FF_SFN_BUF       12
#define FF_FS_RPATH      0

/*---------------------------------------------------------------------------/
/ Drive/Volume Configurations
/---------------------------------------------------------------------------*/
#define FF_VOLUMES       1
#define FF_STR_VOLUME_ID 0
#define FF_MULTI_PARTITION 0
#define FF_MIN_SS        512
#define FF_MAX_SS        512
#define FF_LBA64         0
#define FF_FS_EXFAT      0   /* 只定义一次 */
#define FF_FS_NORTC      0
#define FF_NORTC_MON     1
#define FF_NORTC_MDAY    1
#define FF_NORTC_YEAR    2026
#define FF_FS_NOFSINFO   0

/*---------------------------------------------------------------------------/
/ System Configurations
/---------------------------------------------------------------------------*/
#define FF_FS_TINY       0
#define FF_FS_LOCK       4

/*
 * 重入保护（线程安全）
 * FF_SYNC_t = SemaphoreHandle_t，由 ffsystem.c 实现互斥锁
 * FF_FS_TIMEOUT 单位为 FreeRTOS tick（1000 = 1秒）
 */
#define FF_FS_REENTRANT  1
#define FF_FS_TIMEOUT    1000
#define FF_SYNC_t        SemaphoreHandle_t

#endif /* FFCONF_DEF */
