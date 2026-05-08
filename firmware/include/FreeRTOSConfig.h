#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

/**
 * @file FreeRTOSConfig.h
 * @brief FreeRTOS 配置 - MK64FN1M0VLL12 (ARM Cortex-M4, 120MHz)
 */

/* -----------------------------------------------------------------------
 * 基础配置
 * ----------------------------------------------------------------------- */
#define configUSE_PREEMPTION                    1
#define configUSE_IDLE_HOOK                     1
#define configUSE_TICK_HOOK                     0
#define configUSE_MALLOC_FAILED_HOOK            1
#define configCHECK_FOR_STACK_OVERFLOW          2   /* 方法2：更可靠 */
#define configCPU_CLOCK_HZ                      120000000UL
#define configTICK_RATE_HZ                      1000U   /* 1ms tick */
#define configMAX_PRIORITIES                    6U
#define configMINIMAL_STACK_SIZE                128U    /* 最小栈 512 字节 */
#define configTOTAL_HEAP_SIZE                   (64U * 1024U)  /* 64KB 堆 */
#define configMAX_TASK_NAME_LEN                 12U
#define configUSE_TRACE_FACILITY                0
#define configUSE_STATS_FORMATTING_FUNCTIONS    0
#define configUSE_16_BIT_TICKS                  0
#define configIDLE_SHOULD_YIELD                 1
#define configUSE_TASK_NOTIFICATIONS            1
#define configTASK_NOTIFICATION_ARRAY_ENTRIES   1
#define configUSE_MUTEXES                       1
#define configUSE_RECURSIVE_MUTEXES             0
#define configUSE_COUNTING_SEMAPHORES           1
#define configQUEUE_REGISTRY_SIZE               8U
#define configUSE_QUEUE_SETS                    0
#define configUSE_TIME_SLICING                  1
#define configUSE_NEWLIB_REENTRANT              0
#define configENABLE_BACKWARD_COMPATIBILITY     0
#define configNUM_THREAD_LOCAL_STORAGE_POINTERS 0
#define configSTACK_DEPTH_TYPE                  uint16_t
#define configMESSAGE_BUFFER_LENGTH_TYPE        size_t

/* -----------------------------------------------------------------------
 * 内存分配
 * ----------------------------------------------------------------------- */
#define configSUPPORT_STATIC_ALLOCATION         0
#define configSUPPORT_DYNAMIC_ALLOCATION        1

/* -----------------------------------------------------------------------
 * 软件定时器
 * ----------------------------------------------------------------------- */
#define configUSE_TIMERS                        1
#define configTIMER_TASK_PRIORITY               (configMAX_PRIORITIES - 1U)
#define configTIMER_QUEUE_LENGTH                10U
#define configTIMER_TASK_STACK_DEPTH            256U

/* -----------------------------------------------------------------------
 * 协程（不使用）
 * ----------------------------------------------------------------------- */
#define configUSE_CO_ROUTINES                   0
#define configMAX_CO_ROUTINE_PRIORITIES         1

/* -----------------------------------------------------------------------
 * 可选 API 函数
 * ----------------------------------------------------------------------- */
#define INCLUDE_vTaskPrioritySet                1
#define INCLUDE_uxTaskPriorityGet               1
#define INCLUDE_vTaskDelete                     1
#define INCLUDE_vTaskSuspend                    1
#define INCLUDE_xResumeFromISR                  1
#define INCLUDE_vTaskDelayUntil                 1
#define INCLUDE_vTaskDelay                      1
#define INCLUDE_xTaskGetSchedulerState          1
#define INCLUDE_xTaskGetCurrentTaskHandle       1
#define INCLUDE_uxTaskGetStackHighWaterMark     1
#define INCLUDE_xTaskGetIdleTaskHandle          0
#define INCLUDE_eTaskGetState                   0
#define INCLUDE_xEventGroupSetBitFromISR        1
#define INCLUDE_xTimerPendFunctionCall          0
#define INCLUDE_xTaskAbortDelay                 0
#define INCLUDE_xTaskGetHandle                  0
#define INCLUDE_xTaskResumeFromISR              1

/* -----------------------------------------------------------------------
 * Cortex-M4 中断优先级配置
 * MK64 使用 4 位优先级（0-15），数值越小优先级越高
 * ----------------------------------------------------------------------- */
#ifdef __NVIC_PRIO_BITS
    #define configPRIO_BITS __NVIC_PRIO_BITS
#else
    #define configPRIO_BITS 4U
#endif

/* FreeRTOS 使用的最低中断优先级（数值最大） */
#define configLIBRARY_LOWEST_INTERRUPT_PRIORITY         15U
/* FreeRTOS 可管理的最高中断优先级（数值最小，但不能为 0） */
#define configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY    5U

#define configKERNEL_INTERRUPT_PRIORITY \
    (configLIBRARY_LOWEST_INTERRUPT_PRIORITY << (8U - configPRIO_BITS))
#define configMAX_SYSCALL_INTERRUPT_PRIORITY \
    (configLIBRARY_MAX_SYSCALL_INTERRUPT_PRIORITY << (8U - configPRIO_BITS))

/* -----------------------------------------------------------------------
 * 断言
 * ----------------------------------------------------------------------- */
#define configASSERT(x) \
    do { if ((x) == 0) { taskDISABLE_INTERRUPTS(); for(;;){} } } while(0)

/* -----------------------------------------------------------------------
 * Cortex-M4 异常处理映射到 FreeRTOS
 * ----------------------------------------------------------------------- */
#define vPortSVCHandler     SVC_Handler
#define xPortPendSVHandler  PendSV_Handler
#define xPortSysTickHandler SysTick_Handler

#endif /* FREERTOS_CONFIG_H */
