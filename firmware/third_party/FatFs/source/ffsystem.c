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
 * ff_mutex_create
 * 为卷 vol 创建一个互斥锁，返回 1 表示成功，0 表示失败。
 * FatFs 在初始化时（f_mount）调用此函数。
 */
int ff_mutex_create(int vol)
{
    /* Mutex[vol] 是 FatFs 内部维护的 FF_SYNC_t 数组元素 */
    /* 通过 FatFs 内部宏 Mutex[vol] 访问，此处直接操作 */
    extern FF_SYNC_t Mutex[FF_VOLUMES + 1];

    Mutex[vol] = xSemaphoreCreateMutex();
    return (Mutex[vol] != NULL) ? 1 : 0;
}

/*
 * ff_mutex_delete
 * 删除卷 vol 的互斥锁。
 * FatFs 在卸载（f_mount with NULL）时调用。
 */
void ff_mutex_delete(int vol)
{
    extern FF_SYNC_t Mutex[FF_VOLUMES + 1];

    if (Mutex[vol] != NULL) {
        vSemaphoreDelete(Mutex[vol]);
        Mutex[vol] = NULL;
    }
}

/*
 * ff_mutex_take
 * 获取卷 vol 的互斥锁，超时 FF_FS_TIMEOUT tick。
 * 返回 1 表示成功获取，0 表示超时。
 */
int ff_mutex_take(int vol)
{
    extern FF_SYNC_t Mutex[FF_VOLUMES + 1];

    if (Mutex[vol] == NULL) return 0;
    return (xSemaphoreTake(Mutex[vol], FF_FS_TIMEOUT) == pdTRUE) ? 1 : 0;
}

/*
 * ff_mutex_give
 * 释放卷 vol 的互斥锁。
 */
void ff_mutex_give(int vol)
{
    extern FF_SYNC_t Mutex[FF_VOLUMES + 1];

    if (Mutex[vol] != NULL) {
        xSemaphoreGive(Mutex[vol]);
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
