/*------------------------------------------------------------------------*/
/* ffsystem.c — FatFs OS 依赖层（FreeRTOS 适配，嵌入式专用）              */
/*                                                                        */
/* 重要说明：                                                              */
/*   此文件是项目自有实现，专为 FreeRTOS + ARM Cortex-M4 编写。           */
/*   如果你从 FatFs 官方包下载了 ffsystem.c，请用此文件覆盖它，           */
/*   否则官方版本会尝试 #include <windows.h> 导致编译失败。               */
/*                                                                        */
/* ffconf.h 相关配置：                                                    */
/*   #define FF_FS_REENTRANT  1                                           */
/*   #define FF_FS_TIMEOUT    1000                                        */
/*   #define FF_SYNC_t        SemaphoreHandle_t                           */
/*------------------------------------------------------------------------*/

/* 屏蔽官方 ffsystem.c 中可能存在的平台检测宏，防止误引用 windows.h */
#if defined(_WIN32)
#  undef _WIN32
#endif

#include "ff.h"
#include "FreeRTOS.h"
#include "semphr.h"

/* 编译期检查：确保使用了正确的配置 */
#ifndef FF_FS_REENTRANT
#  error "FF_FS_REENTRANT must be defined in ffconf.h"
#endif

#ifndef FF_SYNC_t
#  error "FF_SYNC_t must be defined as SemaphoreHandle_t in ffconf.h"
#endif

#if FF_FS_REENTRANT

/*
 * FatFs 内部维护的互斥锁数组，声明为 extern 以便访问。
 * 数组大小为 FF_VOLUMES + 1（最后一项用于文件锁）。
 */
extern FF_SYNC_t Mutex[FF_VOLUMES + 1];

/*
 * ff_mutex_create — 为卷 vol 创建互斥锁
 * 返回 1 成功，0 失败
 */
int ff_mutex_create(int vol)
{
    Mutex[vol] = xSemaphoreCreateMutex();
    return (Mutex[vol] != NULL) ? 1 : 0;
}

/*
 * ff_mutex_delete — 删除卷 vol 的互斥锁
 */
void ff_mutex_delete(int vol)
{
    if (Mutex[vol] != NULL) {
        vSemaphoreDelete(Mutex[vol]);
        Mutex[vol] = NULL;
    }
}

/*
 * ff_mutex_take — 获取互斥锁，超时 FF_FS_TIMEOUT tick
 * 返回 1 成功，0 超时
 */
int ff_mutex_take(int vol)
{
    if (Mutex[vol] == NULL) {
        return 0;
    }
    return (xSemaphoreTake(Mutex[vol], (TickType_t)FF_FS_TIMEOUT) == pdTRUE) ? 1 : 0;
}

/*
 * ff_mutex_give — 释放互斥锁
 */
void ff_mutex_give(int vol)
{
    if (Mutex[vol] != NULL) {
        xSemaphoreGive(Mutex[vol]);
    }
}

#endif /* FF_FS_REENTRANT */


/*------------------------------------------------------------------------*/
/* 动态内存分配（FF_USE_LFN == 3 时启用，使用 FreeRTOS 堆）               */
/*------------------------------------------------------------------------*/

#if FF_USE_LFN == 3

void* ff_memalloc(UINT msize)
{
    return pvPortMalloc((size_t)msize);
}

void ff_memfree(void* mblock)
{
    vPortFree(mblock);
}

#endif /* FF_USE_LFN == 3 */
