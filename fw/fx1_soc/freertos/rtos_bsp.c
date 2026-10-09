#include "rtos_bsp.h"

#include "fx1_fw.h"
#include "queue.h"
#include "task.h"

#define UART(offset)  (FX1_UART_BASE + (offset))
#define UART_DR       0x000u
#define UART_FR       0x018u
#define UART_IMSC     0x038u
#define UART_ICR      0x044u
#define FR_RXFE       0x10u
#define INT_RX        0x10u
#define INT_RT        0x40u

extern void freertos_risc_v_trap_handler(void);

static bsp_irq_handler handlers[FX1_PLIC_NUM_SOURCES];
static QueueHandle_t rx_queue;

void freertos_risc_v_application_interrupt_handler(uint32_t mcause)
{
    if (mcause != (FX1_MCAUSE_INTERRUPT | FX1_IRQ_M_EXT)) {
        fx1_log_hex("rtos: unexpected interrupt, mcause", mcause);
        fx1_fail(0xE200);
    }
    BaseType_t woken = pdFALSE;
    const uint32_t id = fx1_plic_claim(0);
    if (id && id < FX1_PLIC_NUM_SOURCES && handlers[id]) woken = handlers[id]();
    if (id) fx1_plic_complete(0, id);
    portYIELD_FROM_ISR(woken);
}

/* probe.S: the CLI's fault-tolerant load and store instructions. */
extern const char fx1_probe_load_insn[], fx1_probe_store_insn[];
extern volatile uint32_t fx1_probe_fault;

void freertos_risc_v_application_exception_handler(uint32_t mcause)
{
    uint32_t mepc, mtval;
    __asm__ volatile("csrr %0, mepc" : "=r"(mepc));
    __asm__ volatile("csrr %0, mtval" : "=r"(mtval));
    /* A CLI probe faulted (load/store access fault or misaligned): flag it and
     * return; the port resumes at mepc + 4, the instruction after the probe. */
    if ((mepc == (uint32_t)(uintptr_t)fx1_probe_load_insn && (mcause == 4u || mcause == 5u)) ||
        (mepc == (uint32_t)(uintptr_t)fx1_probe_store_insn && (mcause == 6u || mcause == 7u))) {
        fx1_probe_fault = 1u;
        return;
    }
    fx1_log_hex("rtos: exception, mcause", mcause);
    fx1_log_hex("  mepc", mepc);
    fx1_log_hex("  mtval", mtval);
    fx1_fail(0xE100 | (mcause & 0xFFu));
}

void fx1_rtos_assert(const char* file, int line)
{
    fx1_puts("[h0] configASSERT failed: ");
    fx1_puts(file);
    fx1_puts(":");
    fx1_putdec((uint32_t)line);
    fx1_puts("\n");
    fx1_fail(0xE300);
}

void vApplicationStackOverflowHook(TaskHandle_t task, char* name)
{
    (void)task;
    fx1_puts("[h0] stack overflow in task ");
    fx1_puts(name);
    fx1_puts("\n");
    fx1_fail(0xE301);
}

void vApplicationMallocFailedHook(void)
{
    fx1_log("malloc failed");
    fx1_fail(0xE302);
}

void vApplicationIdleHook(void)
{
    fx1_wfi(); /* any interrupt (tick, UART, devices) wakes the hart */
}

static BaseType_t uart_isr(void)
{
    BaseType_t woken = pdFALSE;
    while (!(fx1_read32(UART(UART_FR)) & FR_RXFE)) {
        const char c = (char)(fx1_read32(UART(UART_DR)) & 0xFFu);
        xQueueSendFromISR(rx_queue, &c, &woken); /* a full queue drops the byte */
    }
    fx1_write32(UART(UART_ICR), INT_RX | INT_RT); /* clear at the device before the PLIC complete */
    return woken;
}

void bsp_irq_register(uint32_t source, bsp_irq_handler handler)
{
    configASSERT(source > 0 && source < FX1_PLIC_NUM_SOURCES);
    handlers[source] = handler;
    fx1_plic_set_priority(source, 1);
    fx1_plic_enable(0, source, 1);
}

void bsp_init(void)
{
    /* The kernel's trap entry replaces the bare-metal one, on hart 0 only
     * (mtvec is per hart; hart 1 keeps crt0's). Interrupts are still masked. */
    __asm__ volatile("csrw mtvec, %0" ::"r"(&freertos_risc_v_trap_handler));
    rx_queue = xQueueCreate(1024, sizeof(char)); /* a replayed script arrives faster than commands run */
    configASSERT(rx_queue);
    fx1_irq_enable(FX1_MIE_MEIE); /* the port enables MTIE when the scheduler starts */
}

void bsp_console_start(void)
{
    bsp_irq_register(FX1_IRQ_UART, uart_isr);
    fx1_write32(UART(UART_IMSC), INT_RX | INT_RT);
}

int bsp_getc(TickType_t wait)
{
    char c;
    return xQueueReceive(rx_queue, &c, wait) == pdTRUE ? (unsigned char)c : -1;
}
