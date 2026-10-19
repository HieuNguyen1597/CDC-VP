/*
 * Kiểm tra cấu hình CVA6 trên VP:
 *   - misa = RV32IMC, chỉ M-mode (Table 8-1);
 *   - lệnh AMO báo illegal instruction (RVA = 0);
 *   - truy cập vùng reserved -> load/store access fault;
 *   - ngắt PLIC -> machine external interrupt, cause 11 (Table 11-2).
 *
 * Kết quả ghi vào fu3_result ở 0x0100_0000 (đầu DSRAM). status = FU3_STATUS_PASS <=> Khi mọi kiểm tra bắt buộc đạt. 
 * Kiểm tra IRQ chỉ có kết quả khi platform/test Assert đường PLIC sau khi thấy irq_request = 1. Nếu không ai Assert thì irq_result = IRQ_NOT_TESTED và không tính là lỗi.
 */

#include <stdint.h>

#define FU3_RESULT_MAGIC    0x46553353u 
#define FU3_STATUS_RUNNING  0x00000000u
#define FU3_STATUS_PASS     0x600D600Du
#define FU3_STATUS_FAIL     0xBAD0BAD0u

#define MISA_RV32IMC        ((1u << 30) | (1u << 12) | (1u << 8) | (1u << 2))
#define MCAUSE_ILLEGAL      2u
#define MCAUSE_LOAD_FAULT   5u
#define MCAUSE_STORE_FAULT  7u
#define MCAUSE_M_EXT_IRQ    0x8000000Bu
#define MCAUSE_INTERRUPT    0x80000000u
#define MIE_MEIE            (1u << 11)
#define MSTATUS_MIE         (1u << 3)

// Vùng Reserved trong memory map => Nus phải trả Error
#define FU3_UNMAPPED_ADDR   0x20000000u

#define IRQ_WAIT_LOOPS      100000u

enum {
    IRQ_NOT_TESTED  = 0,
    IRQ_OK          = 1,
    IRQ_WRONG_CAUSE = 2,
};

typedef struct {
    uint32_t magic;
    uint32_t status;
    uint32_t misa;
    uint32_t cause_amo;
    uint32_t cause_load;
    uint32_t cause_store;
    uint32_t irq_request;   // FW đặt 1: platform/test assert PLIC line (cause 11)
    uint32_t irq_cause;
    uint32_t irq_result;
} fu3_result_t;

volatile fu3_result_t fu3_result __attribute__((section(".results")));

static volatile uint32_t last_exc_cause;

#define csr_read(csr)                                                \
    ({                                                               \
        uint32_t v_;                                                 \
        __asm__ volatile("csrr %0, " #csr : "=r"(v_));               \
        v_;                                                          \
    })
#define csr_write(csr, v) __asm__ volatile("csrw " #csr ", %0" ::"r"(v))
#define csr_set(csr, v)   __asm__ volatile("csrs " #csr ", %0" ::"r"(v))
#define csr_clear(csr, v) __asm__ volatile("csrc " #csr ", %0" ::"r"(v))

void __attribute__((interrupt("machine"), aligned(4))) fu3_trap_handler(void)
{
    uint32_t mcause = csr_read(mcause);

    if (mcause & MCAUSE_INTERRUPT) {
        fu3_result.irq_cause = mcause;
        // PLIC offset TBD -> chưa có Driver. Tạm clear MEIE chặn ngắt.
        csr_clear(mie, MIE_MEIE);
        return;
    }

    // Ghi lại cause rồi bỏ qua lệnh lỗi (lệnh 16 hoặc 32 bit)
    last_exc_cause = mcause;
    uint32_t mepc = csr_read(mepc);
    uint16_t insn = *(volatile uint16_t *)mepc;
    csr_write(mepc, mepc + (((insn & 3u) == 3u) ? 4u : 2u));
}

static uint32_t trap_cause_of_amo(void)
{
    uint32_t word = 0;
    uint32_t old;

    last_exc_cause = 0;
    // FW build rv32imc nên phải bật tạm A để Assembler chấp nhận lệnh
    __asm__ volatile(".option push\n"
                     ".option arch, +a\n"
                     "amoadd.w %0, %1, (%2)\n"
                     ".option pop\n"
                     : "=r"(old)
                     : "r"(1u), "r"(&word)
                     : "memory");
    (void)old;
    return last_exc_cause;
}

static uint32_t trap_cause_of_load(uint32_t addr)
{
    uint32_t value;

    last_exc_cause = 0;
    __asm__ volatile("lw %0, 0(%1)" : "=r"(value) : "r"(addr) : "memory");
    (void)value;
    return last_exc_cause;
}

static uint32_t trap_cause_of_store(uint32_t addr)
{
    last_exc_cause = 0;
    __asm__ volatile("sw %0, 0(%1)" ::"r"(0u), "r"(addr) : "memory");
    return last_exc_cause;
}

static uint32_t run_irq_check(void)
{
    fu3_result.irq_cause = 0;
    csr_set(mie, MIE_MEIE);
    csr_set(mstatus, MSTATUS_MIE);
    fu3_result.irq_request = 1;

    for (uint32_t i = 0; i < IRQ_WAIT_LOOPS && fu3_result.irq_cause == 0; ++i) {
    }

    csr_clear(mstatus, MSTATUS_MIE);
    csr_clear(mie, MIE_MEIE);
    fu3_result.irq_request = 0;

    if (fu3_result.irq_cause == 0) {
        return IRQ_NOT_TESTED;
    }
    return fu3_result.irq_cause == MCAUSE_M_EXT_IRQ ? IRQ_OK : IRQ_WRONG_CAUSE;
}

int main(void)
{
    int ok = 1;

    fu3_result.magic = FU3_RESULT_MAGIC;
    fu3_result.status = FU3_STATUS_RUNNING;

    fu3_result.misa = csr_read(misa);
    ok &= fu3_result.misa == MISA_RV32IMC;

    fu3_result.cause_amo = trap_cause_of_amo();
    ok &= fu3_result.cause_amo == MCAUSE_ILLEGAL;

    fu3_result.cause_load = trap_cause_of_load(FU3_UNMAPPED_ADDR);
    ok &= fu3_result.cause_load == MCAUSE_LOAD_FAULT;

    fu3_result.cause_store = trap_cause_of_store(FU3_UNMAPPED_ADDR);
    ok &= fu3_result.cause_store == MCAUSE_STORE_FAULT;

    fu3_result.irq_result = run_irq_check();
    ok &= fu3_result.irq_result != IRQ_WRONG_CAUSE;

    fu3_result.status = ok ? FU3_STATUS_PASS : FU3_STATUS_FAIL;
    return 0;
}
