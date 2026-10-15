/*
 * DDR3 errors seen by the CPU, and their recovery (DDR3 VP subtask 5).
 * BootROM image, --ddr-boot firmware. Faults are injected by the host only
 * (DEC-04): the ctest registration passes the same schedule as --ddr3-fault
 * options that this file receives as FAULT_*_MS defines (simulated time from CPU
 * release, which is t = 0 in firmware boot).
 *
 *  A  bring-up (BootROM), test data written, interrupts routed through PLIC 8
 *  B  correctable error (one stored bit flipped): corrected data, CE_COUNT,
 *     ECC_LAST_*, IRQ_RAW[0] (CE_THRESHOLD = 1)
 *  C  uncorrectable error (two bits of one lane): load access fault, UE_COUNT,
 *     IRQ_RAW[1]
 *  D  read-return timeout: load access fault after READ_TIMEOUT, read quarantine
 *     (QUARANTINE_STATUS, IRQ_RAW[2]); reads keep failing
 *  E  recovery: cold reset through DDR_SYSCTRL, QoS sideband again, full
 *     initialization; quarantine cleared, memory scrubbed and usable
 *  F  fatal event (DFI protocol fault): hart 0's next DDR read is held without a
 *     response; hart 1 sees FATAL_STATUS, waits until the read is held
 *     (DDR_SYSCTRL STATUS.fe_drained = 0) and requests a coordinated core reset,
 *     which aborts the held read (load access fault on hart 0); FATAL_STATUS stays
 *     until a cold reset, then initialization succeeds again
 *
 * Fail codes: 0x8x A, 0x9x B, 0xAx C, 0xBx D, 0xCx E, 0xDx F.
 */
#include "ddr3_fw.h"

#ifndef FAULT_CE_MS
#error "FAULT_CE_MS, FAULT_UE_MS, FAULT_TIMEOUT_MS, FAULT_FATAL_MS and the addresses come from CMake"
#endif

#define CE_ADDR 0x80100000u /* --ddr3-fault FAULT_CE_MS:ce:CE_ADDR */
#define UE_ADDR 0x80100040u /* --ddr3-fault FAULT_UE_MS:ue:UE_ADDR */
#define QT_ADDR 0x80100080u
#define IRQ_ENABLED ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 11))
#define AFTER_US 20u /* margin after a scheduled injection */

static volatile uint32_t irq_seen;
static volatile uint32_t hart1_fatal_seen, hart1_irq_raw, hart1_reset_status, hart1_done, hart0_reading;

static uint32_t handler(uint32_t mcause, uint32_t mepc, uint32_t mtval)
{
    uint32_t resume;
    if (ddr3_probe_trap(mcause, mepc, &resume)) return resume;
    if (mcause == (FX1_MCAUSE_INTERRUPT | FX1_IRQ_M_EXT)) {
        const uint32_t ctx = FX1_PLIC_CONTEXT_HART_M(fx1_hartid());
        const uint32_t source = fx1_plic_claim(ctx);
        if (source == FX1_IRQ_DDR) {
            const uint32_t pending = ddr3_rd(FX1_DDR3_IRQ_STATUS);
            irq_seen |= pending;
            ddr3_wr(FX1_DDR3_IRQ_CLEAR, pending);
        }
        if (source) fx1_plic_complete(ctx, source);
        return mepc;
    }
    fx1_log_hex("unexpected trap, mcause", mcause);
    fx1_log_hex("  mepc", mepc);
    fx1_log_hex("  mtval", mtval);
    fx1_fail(0xE000u | (mcause & 0xFFu));
}

static void expect_eq(uint32_t got, uint32_t want, uint32_t code, const char* what)
{
    if (got != want) {
        fx1_log_hex("  got ", got);
        fx1_log_hex("  want", want);
    }
    fx1_check(got == want, code, what);
}

static void wait_injection(uint32_t ms, uint32_t code)
{
    fx1_check(fx1_mtime() < ddr3_ms_to_ticks(ms), code, "still before the scheduled injection");
    ddr3_sleep_until(ddr3_ms_to_ticks(ms) + AFTER_US * DDR3_TICKS_PER_US);
}

static void route_interrupts(void)
{
    ddr3_wr(FX1_DDR3_CE_THRESHOLD, 1u);
    ddr3_wr(FX1_DDR3_IRQ_CLEAR, 0xFFFFFFFFu);
    ddr3_wr(FX1_DDR3_IRQ_MASK, IRQ_ENABLED);
    irq_seen = 0;
}

static void bringup(uint32_t code, const char* what)
{
    const ddr3_status_t s = ddr3_bringup(2000u);
    if (s != DDR3_OK) ddr3_log_status(what, s);
    fx1_check(s == DDR3_OK, code, what);
    route_interrupts();
}

static void write_data(void)
{
    for (uint32_t a = CE_ADDR; a < CE_ADDR + 0xC0u; a += 4) fx1_write32(a, a ^ 0x5A5A0000u);
}

void secondary_main(uint32_t hart)
{
    (void)hart;
    /* Phase F agent: watch FATAL_STATUS (CSR path, never held). Reset the core only once
     * hart 0's read is held in the controller (DDR_SYSCTRL STATUS.fe_drained = 0): a reset
     * before it arrives would leave it waiting for a readiness that never comes. */
    while (!ddr3_rd(FX1_DDR3_FATAL_STATUS)) ddr3_sleep_us(5u);
    hart1_fatal_seen = ddr3_rd(FX1_DDR3_FATAL_STATUS);
    hart1_irq_raw = ddr3_rd(FX1_DDR3_IRQ_RAW);
    const uint64_t limit = fx1_mtime() + ddr3_ms_to_ticks(1);
    while (!hart0_reading || (ddrsc_rd(FX1_DDRSC_STATUS) & FX1_DDRSC_ST_FE_DRAINED)) {
        if (fx1_mtime() > limit) fx1_fail(0xDE);
        ddr3_sleep_us(1u);
    }
    hart1_reset_status = ddr3_reset(FX1_DDRSC_RST_CORE);
    fx1_release();
    hart1_done = 1;
    for (;;) fx1_wfi();
}

int main(void)
{
    fx1_set_trap_handler(handler);
    fx1_log("DDR3 errors and recovery on the CPU (host fault schedule)");

    /* A */
    bringup(0x80, "A: bring-up");
    write_data();
    fx1_plic_set_priority(FX1_IRQ_DDR, 1);
    fx1_plic_set_threshold(FX1_PLIC_CONTEXT_HART_M(0), 0);
    fx1_plic_enable(FX1_PLIC_CONTEXT_HART_M(0), FX1_IRQ_DDR, 1);
    fx1_irq_enable(FX1_MIE_MEIE);
    fx1_global_irq_enable();
    fx1_log_hex("A: ready, test data written; ms since release", (uint32_t)(fx1_mtime() / ddr3_ms_to_ticks(1)));

    /* B: correctable */
    wait_injection(FAULT_CE_MS, 0x90);
    expect_eq(fx1_read32(CE_ADDR), CE_ADDR ^ 0x5A5A0000u, 0x91, "B: corrected data returned");
    expect_eq(ddr3_snap(FX1_DDR3_CE_COUNT), 1, 0x92, "B: CE_COUNT 1");
    expect_eq(ddr3_rd(FX1_DDR3_ECC_LAST_ADDR_LO), (CE_ADDR - FX1_DDR_BASE) / 32u, 0x93, "B: ECC_LAST_ADDR = granule");
    expect_eq(ddr3_rd(FX1_DDR3_ECC_LAST_INFO) & 3u, 1u, 0x94, "B: ECC_LAST_INFO type CE");
    ddr3_sleep_us(5);
    fx1_check(irq_seen & (1u << 0), 0x95, "B: IRQ_RAW[0] through PLIC");
    fx1_log("B: correctable error corrected, counted, interrupt");

    /* C: uncorrectable */
    wait_injection(FAULT_UE_MS, 0xA0);
    uint32_t v = 0xFFFFFFFFu;
    expect_eq(ddr3_probe_read32(UE_ADDR, &v), 5u, 0xA1, "C: uncorrectable read -> load access fault");
    expect_eq(ddr3_snap(FX1_DDR3_UE_COUNT), 1, 0xA2, "C: UE_COUNT 1");
    expect_eq(ddr3_rd(FX1_DDR3_ECC_LAST_INFO) & 3u, 2u, 0xA3, "C: ECC_LAST_INFO type UE");
    ddr3_sleep_us(5);
    fx1_check(irq_seen & (1u << 1), 0xA4, "C: IRQ_RAW[1] through PLIC");
    expect_eq(fx1_read32(UE_ADDR + 0x20u), (UE_ADDR + 0x20u) ^ 0x5A5A0000u, 0xA5, "C: the next granule is intact");
    fx1_log("C: uncorrectable error -> access fault, counted, interrupt");

    /* D: read-return timeout -> quarantine */
    wait_injection(FAULT_TIMEOUT_MS, 0xB0);
    const uint64_t t0 = fx1_mtime();
    expect_eq(ddr3_probe_read32(QT_ADDR, &v), 5u, 0xB1, "D: lost read return -> load access fault");
    fx1_log_hex("D: fault after ticks", (uint32_t)(fx1_mtime() - t0));
    const uint32_t q = ddr3_rd(FX1_DDR3_QUARANTINE_STATUS);
    expect_eq(q & FX1_DDR3_QUAR_ST_ACTIVE, FX1_DDR3_QUAR_ST_ACTIVE, 0xB2, "D: QUARANTINE_STATUS active");
    expect_eq(FX1_DDR3_QUAR_ST_REASON(q), 0, 0xB3, "D: reason 0 (timeout)");
    ddr3_sleep_us(5);
    fx1_check(irq_seen & (1u << 2), 0xB4, "D: IRQ_RAW[2] through PLIC");
    expect_eq(ddr3_probe_read32(CE_ADDR, &v), 5u, 0xB5, "D: later reads fail while quarantined");
    fx1_log("D: read timeout -> quarantine");

    /* E: recovery needs a cold reset and a full initialization */
    fx1_global_irq_disable();
    uint32_t rs = ddr3_reset(FX1_DDRSC_RST_COLD);
    fx1_check((rs & FX1_DDRSC_RST_DONE) && FX1_DDRSC_RST_COLD_COUNT(rs) == 1, 0xC0, "E: cold reset done");
    expect_eq(ddr3_rd(FX1_DDR3_QUARANTINE_STATUS) & FX1_DDR3_QUAR_ST_ACTIVE, 0, 0xC1, "E: quarantine cleared");
    expect_eq(ddrsc_rd(FX1_DDRSC_QOS_CFG), 0, 0xC2, "E: the cold reset cleared the QoS sideband");
    bringup(0xC3, "E: re-initialization");
    expect_eq(fx1_read32(CE_ADDR), 0, 0xC4, "E: memory scrubbed by the new initialization");
    fx1_write32(QT_ADDR, 0x12345678u);
    expect_eq(fx1_read32(QT_ADDR), 0x12345678u, 0xC5, "E: memory usable again");
    expect_eq(ddr3_snap(FX1_DDR3_UE_COUNT), 0, 0xC6, "E: counters reset");
    fx1_global_irq_enable();
    fx1_log_hex("E: cold reset + re-initialization; ms since release", (uint32_t)(fx1_mtime() / ddr3_ms_to_ticks(1)));

    /* F: fatal hold, core reset by hart 1, cold reset */
    fx1_release_secondaries();
    wait_injection(FAULT_FATAL_MS, 0xD0);
    hart0_reading = 1;
    fx1_mb();
    expect_eq(ddr3_probe_read32(QT_ADDR, &v), 5u, 0xD1, "F: held read aborted by the core reset -> access fault");
    while (!hart1_done) ddr3_sleep_us(5);
    fx1_acquire();
    fx1_check(hart1_fatal_seen & FX1_DDR3_FATAL_DFI_PROTOCOL, 0xD2, "F: hart 1 saw FATAL_STATUS[3]");
    fx1_check((hart1_reset_status & FX1_DDRSC_RST_DONE) && FX1_DDRSC_RST_CORE_COUNT(hart1_reset_status) == 1, 0xD3,
              "F: hart 1's coordinated core reset completed");
    fx1_check(hart1_irq_raw & (1u << 11), 0xD4, "F: IRQ_RAW[11] (fatal containment) seen by hart 1");
    fx1_check(ddr3_rd(FX1_DDR3_FATAL_STATUS) & FX1_DDR3_FATAL_DFI_PROTOCOL, 0xD5, "F: FATAL_STATUS survives the core reset");
    fx1_global_irq_disable();
    rs = ddr3_reset(FX1_DDRSC_RST_COLD);
    fx1_check(FX1_DDRSC_RST_COLD_COUNT(rs) == 2, 0xD6, "F: second cold reset");
    expect_eq(ddr3_rd(FX1_DDR3_FATAL_STATUS), 0, 0xD7, "F: the cold reset clears FATAL_STATUS");
    bringup(0xD8, "F: re-initialization after the fatal event");
    fx1_write32(QT_ADDR, 0xCAFEF00Du);
    expect_eq(fx1_read32(QT_ADDR), 0xCAFEF00Du, 0xD9, "F: memory usable again");
    fx1_log("F: fatal hold -> core reset by hart 1 -> cold reset -> re-initialization");
    return 0;
}
