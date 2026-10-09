/*
 * FreeRTOS V11.2.0 configuration for the FX1 SoC VP (fw/fx1_soc/freertos).
 *
 * AMP: the kernel runs on hart 0 only; hart 1 runs the bare-metal boot IP
 * check (hwcheck.c) and then parks. Machine mode, no MPU.
 *
 * Tick: the CLINT mtime counter runs at FX1_MTIME_HZ (20 MHz). The RISC-V
 * port's configCPU_CLOCK_HZ is the *mtime* rate (the port divides it by the
 * tick rate to get the mtimecmp increment), not a CPU frequency.
 */
#ifndef FREERTOS_CONFIG_H
#define FREERTOS_CONFIG_H

#include "fx1/fx1_memory_map.h"

/* CLINT, SiFive layout: mtime at +0xBFF8, mtimecmp[hart] at +0x4000 + 8 * hart
 * (the port adds mhartid * 8 itself). */
#define configMTIME_BASE_ADDRESS    ( FX1_CLINT_BASE + FX1_CLINT_MTIME_OFF )
#define configMTIMECMP_BASE_ADDRESS ( FX1_CLINT_BASE + FX1_CLINT_MTIMECMP_OFF )

#define configCPU_CLOCK_HZ          ( ( unsigned long ) FX1_MTIME_HZ )
#define configTICK_RATE_HZ          ( ( TickType_t ) 1000 )
#define configUSE_PREEMPTION        1
#define configUSE_TIME_SLICING      1
#define configUSE_PORT_OPTIMISED_TASK_SELECTION 0
#define configUSE_IDLE_HOOK         1 /* the idle task executes wfi */
#define configUSE_TICK_HOOK         0
#define configMAX_PRIORITIES        5
#define configMINIMAL_STACK_SIZE    ( ( unsigned short ) 256 ) /* words */
#define configISR_STACK_SIZE_WORDS  1024
#define configTOTAL_HEAP_SIZE       ( ( size_t ) ( 256 * 1024 ) )
#define configMAX_TASK_NAME_LEN     12
#define configUSE_16_BIT_TICKS      0
#define configIDLE_SHOULD_YIELD     1
#define configUSE_MUTEXES           1
#define configUSE_RECURSIVE_MUTEXES 0
#define configUSE_COUNTING_SEMAPHORES 1
#define configQUEUE_REGISTRY_SIZE   0
#define configUSE_QUEUE_SETS        0
#define configUSE_TASK_NOTIFICATIONS 1
#define configCHECK_FOR_STACK_OVERFLOW 2
#define configUSE_MALLOC_FAILED_HOOK 1
#define configUSE_TRACE_FACILITY    1 /* uxTaskGetSystemState for the `tasks` command */
#define configUSE_STATS_FORMATTING_FUNCTIONS 0
#define configGENERATE_RUN_TIME_STATS 0
#define configSUPPORT_STATIC_ALLOCATION 0
#define configSUPPORT_DYNAMIC_ALLOCATION 1
#define configUSE_TIMERS            0
#define configUSE_CO_ROUTINES       0
#define configENABLE_BACKWARD_COMPATIBILITY 0

#define INCLUDE_vTaskDelay          1
#define INCLUDE_vTaskDelete         1
#define INCLUDE_vTaskSuspend        1
#define INCLUDE_xTaskGetSchedulerState 1
#define INCLUDE_uxTaskGetStackHighWaterMark 1
#define INCLUDE_xTaskGetIdleTaskHandle 0
#define INCLUDE_eTaskGetState       1

/* A failed configASSERT() reports through the platform's fatal path. C only:
 * the port's assembly includes this file too. */
#ifndef __ASSEMBLER__
void fx1_rtos_assert(const char* file, int line);
#define configASSERT( x ) do { if( !( x ) ) fx1_rtos_assert( __FILE__, __LINE__ ); } while( 0 )
#endif

#endif /* FREERTOS_CONFIG_H */
