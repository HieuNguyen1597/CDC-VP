/*
 * FreeRTOS board support for the FX1 SoC VP (hart 0).
 *
 *   - PLIC dispatch: freertos_risc_v_application_interrupt_handler claims on
 *     context 0, calls the handler registered for the source, completes. A
 *     handler clears its source at the device and may wake tasks; it returns
 *     whether a higher-priority task was woken (the port then switches).
 *   - Exceptions other than ecall: report and FAIL (fx1_fail).
 *   - UART console: RX interrupt (PLIC source 1) -> queue -> bsp_getc();
 *     TX by polling (fx1_putc). Output from tasks is not interleaved because
 *     only the CLI task prints after boot (fx1_log lines are atomic anyway).
 */
#ifndef RTOS_BSP_H
#define RTOS_BSP_H

#include <stdint.h>

#include "FreeRTOS.h"

typedef BaseType_t (*bsp_irq_handler)(void); /* returns pdTRUE if a task must run */

void bsp_init(void);                                   /* before vTaskStartScheduler */
void bsp_irq_register(uint32_t source, bsp_irq_handler handler);
void bsp_console_start(void);                          /* enables UART RX interrupts */
int bsp_getc(TickType_t wait);                         /* -1 on timeout */

#endif /* RTOS_BSP_H */
