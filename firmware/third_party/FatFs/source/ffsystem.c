/*------------------------------------------------------------------------*/
/* ffsystem.c — FatFs OS 依赖层（FreeRTOS 适配）                          */
/*                                                                        */
/* 当 ffconf.h 中 FF_FS_REENTRANT = 1 时，FatFs 需要以下互斥锁接口：      */
/*   ff_mutex_create  — 创建互斥锁                                        */
/*   ff_mutex_delete  — 删除互斥锁                                        */
/*   ff_mutex_take    — 获取互斥锁（带超时）                               */
/*   ff_mutex_give    — 释放互斥锁                                        */
/*                                                                        */
/* ffconf.h 相关配置：                                                    */
/*   #define FF_FS_REENTRANT  1                                           */
/*   #define FF_FS_TIMEOUT    1000   // 等待超时 tick 数                  */
/*   #define FF_SYNC_t        SemaphoreHandle_t                           */
/*------------------------------------------------------------------------*/

#include "ff.h"
#include "FreeRTOS.h"
#include "semphr.h"

#if FF_FS_REENTRANT  /* 仅在启用重入保护时编译 */

/*
 * 卷互斥锁句柄表。
 * 按 FatFs R0.15 约定，互斥锁句柄由 OS 依赖层（本文件）自行维护，
 * ff.c 仅通过 ff_mutex_take/give(vol) 间接访问，不导出该符号。
 * 因此这里必须在本编译单元内定义为 static，
 * 原先用 `extern FF_SYNC_t Mutex[]` 引用会在链接期报 undefined symbol。
 * 索引 0..FF_VOLUMES-1 为各卷，FF_VOLUMES 处为系统级锁（f_mkfs 等使用）。
 */
static FF_SYNC_t s_ff_mutex[FF_VOLUMES + 1];

/*
 * ff_mutex_create
 * 为卷 vol 创建一个互斥锁，返回 1 表示成功，0 表示失败。
 * FatFs 在初始化时（f_mount）调用此函数。
 */
int ff_mutex_create(int vol)
{
    s_ff_mutex[vol] = xSemaphoreCreateMutex();
    return (s_ff_mutex[vol] != NULL) ? 1 : 0;
}

/*
 * ff_mutex_delete
 * 删除卷 vol 的互斥锁。
 * FatFs 在卸载（f_mount with NULL）时调用。
 */
void ff_mutex_delete(int vol)
{
    if (s_ff_mutex[vol] != NULL) {
        vSemaphoreDelete(s_ff_mutex[vol]);
        s_ff_mutex[vol] = NULL;
    }
}

/*
 * ff_mutex_take
 * 获取卷 vol 的互斥锁，超时 FF_FS_TIMEOUT tick。
 * 返回 1 表示成功获取，0 表示超时。
 */
int ff_mutex_take(int vol)
{
    if (s_ff_mutex[vol] == NULL) return 0;
    return (xSemaphoreTake(s_ff_mutex[vol], FF_FS_TIMEOUT) == pdTRUE) ? 1 : 0;
}

/*
 * ff_mutex_give
 * 释放卷 vol 的互斥锁。
 */
void ff_mutex_give(int vol)
{
    if (s_ff_mutex[vol] != NULL) {
        xSemaphoreGive(s_ff_mutex[vol]);
    }
}

#endif /* FF_FS_REENTRANT */


/*------------------------------------------------------------------------*/
/* 内存分配（可选，FF_USE_LFN == 3 时需要）                                */
/*------------------------------------------------------------------------*/

#if FF_USE_LFN == 3  /* 动态内存分配模式 */
#include <stdlib.h>

void* ff_memalloc(UINT msize)
{
    return pvPortMalloc(msize);   /* 使用 FreeRTOS 堆 */
}

void ff_memfree(void* mblock)
{
    vPortFree(mblock);
}

#endif /* FF_USE_LFN == 3 */
