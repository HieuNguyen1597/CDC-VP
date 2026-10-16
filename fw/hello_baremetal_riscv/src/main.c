/* Bare-metal "hello" for the cdc-vp riscv_cpu_eval platform.
 *
 * Memory map (must match platforms/riscv_cpu_eval): UART TXDATA @ 0x10000000.
 * Writing a byte to TXDATA emits one character through uart_tlm.
 */


// Default: UART của riscv_cpu_eval
// FU3: build với -DUART_BASE=0x440D0000u (UART0)
#ifndef UART_BASE
#define UART_BASE 0x10000000u
#endif
#define UART_TX (*(volatile unsigned char *)UART_BASE)

static void uart_puts(const char *s)
{
    while (*s) {
        UART_TX = (unsigned char)*s++;
    }
}

int main(void)
{
    uart_puts("Hello from RISC-V\n");
    return 0;
}
