/*
 * G6: UART RX from the host (fx1_soc --uart0-rx-file tests/uart_rx/input.txt).
 *   1  polled: wait for bytes with FR.RXFE, read DR, collect "poll line"
 *   2  interrupt: RX + receive-timeout interrupts enabled (IMSC), PLIC source 1
 *      on context 0; the handler drains the FIFO and clears at the UART
 *      (ICR) before completing at the PLIC; collect "irq line"
 * Both lines end with CR, as a terminal sends them. Expected output (ctest
 * regex): "got poll: [poll line]" and "got irq: [irq line]".
 */
#include "fx1_fw.h"

#define UART(offset)  (FX1_UART_BASE + (offset))
#define UART_DR       0x000u
#define UART_FR       0x018u
#define UART_IMSC     0x038u
#define UART_MIS      0x040u
#define UART_ICR      0x044u
#define FR_RXFE       0x10u
#define INT_RX        0x10u
#define INT_RT        0x40u

static char line[64];
static volatile uint32_t len, done, irqs;

static uint32_t handler(uint32_t mcause, uint32_t mepc, uint32_t mtval)
{
    (void)mtval;
    if (mcause != (FX1_MCAUSE_INTERRUPT | FX1_IRQ_M_EXT)) {
        fx1_log_hex("unexpected mcause", mcause);
        fx1_fail(0x0E0);
    }
    const uint32_t id = fx1_plic_claim(0);
    if (id == FX1_IRQ_UART) {
        ++irqs;
        while (!(fx1_read32(UART(UART_FR)) & FR_RXFE)) {
            const char c = (char)(fx1_read32(UART(UART_DR)) & 0xFFu);
            if (c == '\r') done = 1;
            else if (len < sizeof(line) - 1) line[len++] = c;
        }
        fx1_write32(UART(UART_ICR), INT_RX | INT_RT); /* clear at the device first */
    }
    if (id) fx1_plic_complete(0, id);
    return mepc;
}

static void print_line(const char* what)
{
    line[len] = 0;
    fx1_puts("[h0] got ");
    fx1_puts(what);
    fx1_puts(": [");
    fx1_puts(line);
    fx1_puts("]\n");
}

static int same(const char* a, const char* b)
{
    while (*a && *a == *b) ++a, ++b;
    return *a == *b;
}

int main(void)
{
    /* 1: polling */
    const uint64_t t0 = fx1_mtime();
    for (;;) {
        fx1_check(fx1_mtime() - t0 < FX1_MTIME_HZ / 10u, 0x10, "first line within 100 ms");
        if (fx1_read32(UART(UART_FR)) & FR_RXFE) continue;
        const char c = (char)(fx1_read32(UART(UART_DR)) & 0xFFu);
        if (c == '\r') break;
        if (len < sizeof(line) - 1) line[len++] = c;
    }
    print_line("poll");
    line[len] = 0;
    fx1_check(same(line, "poll line"), 0x11, "polled line");

    /* 2: interrupts */
    len = 0;
    fx1_set_trap_handler(handler);
    fx1_plic_set_priority(FX1_IRQ_UART, 1);
    fx1_plic_enable(0, FX1_IRQ_UART, 1);
    fx1_write32(UART(UART_IMSC), INT_RX | INT_RT);
    fx1_irq_enable(FX1_MIE_MEIE);
    fx1_global_irq_enable();
    const uint64_t t1 = fx1_mtime();
    while (!done) fx1_check(fx1_mtime() - t1 < FX1_MTIME_HZ / 10u, 0x20, "second line within 100 ms");
    fx1_global_irq_disable();
    print_line("irq");
    line[len] = 0;
    fx1_check(same(line, "irq line"), 0x21, "interrupt-driven line");
    fx1_check(irqs >= 1, 0x22, "at least one UART interrupt");
    fx1_check(!(fx1_plic_pending() & (1u << FX1_IRQ_UART)), 0x23, "UART line low after the handler");
    fx1_log_hex("UART RX interrupts", irqs);
    return 0;
}
