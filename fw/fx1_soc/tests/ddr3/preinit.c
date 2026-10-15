/*
 * DDR3 in the preinitialized boot (DDR3 VP subtask 5; default --ddr-boot preinit).
 * Ordinary DDR image: the platform initialized the controller, scrubbed it and
 * preloaded this image before releasing the CPUs.
 *
 *  A  state left by the platform: STATUS (preinit, ready), GLOBAL_CTRL.enable,
 *     INIT_STATUS, and the active images equal the firmware reference image
 *     (ddr3_ref_1600.h), so firmware-init and preinit configure the same DDR
 *  B  memory from both harts at once (code, data and stacks are in DDR)
 *  C  correctable error (host fault at FAULT_CE_US after release): corrected data,
 *     CE_COUNT, IRQ_RAW[0] through PLIC source 8
 *  D  uncorrectable error (FAULT_UE_US): load access fault, UE_COUNT, IRQ_RAW[1]
 *  E  self-refresh requested while executing from DDR: the next instruction fetch
 *     wakes the controller (automatic wake), execution continues
 *
 * Fail codes: 0x1x A, 0x2x B, 0x3x C, 0x4x D, 0x5x E.
 */
#include "ddr3_fw.h"
#include "ddr3_ref_1600.h"

#ifndef FAULT_CE_US
#error "FAULT_CE_US and FAULT_UE_US come from CMake"
#endif

#define CE_ADDR    0x90000100u /* --ddr3-fault FAULT_CE_US:ce:CE_ADDR */
#define UE_ADDR    0x90000140u /* --ddr3-fault FAULT_UE_US:ue:UE_ADDR */
#define HART_AREA  0x90100000u /* + 64 KiB per hart */
#define WORDS      2048u

static volatile uint32_t irq_seen;
static volatile uint32_t hart1_done, hart1_errors;
static uint64_t release_ticks;

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

static uint32_t pattern(uint32_t address, uint32_t hart) { return (address * 2246822519u) ^ (hart << 28) ^ 0x3C3Cu; }

static uint32_t memory_pass(uint32_t hart)
{
    volatile uint32_t* w = (volatile uint32_t*)(HART_AREA + 0x10000u * hart);
    for (uint32_t i = 0; i < WORDS; ++i) w[i] = pattern((uint32_t)(uintptr_t)&w[i], hart);
    uint32_t errors = 0;
    for (uint32_t i = 0; i < WORDS; ++i) errors += w[i] != pattern((uint32_t)(uintptr_t)&w[i], hart);
    return errors;
}

void secondary_main(uint32_t hart)
{
    hart1_errors = memory_pass(hart);
    fx1_release();
    hart1_done = 1;
    for (;;) fx1_wfi();
}

static void wait_injection(uint32_t us, uint32_t code)
{
    const uint64_t at = release_ticks + (uint64_t)us * DDR3_TICKS_PER_US;
    fx1_check(fx1_mtime() < at, code, "still before the scheduled injection");
    ddr3_sleep_until(at + 20u * DDR3_TICKS_PER_US);
}

int main(void)
{
    /* The CPUs start right after the release: the first instructions run within a few
     * microseconds of it, well inside the margin of the fault schedule. */
    release_ticks = fx1_mtime();
    fx1_set_trap_handler(handler);
    fx1_log("DDR3 preinitialized boot, FX1 VP");

    /* A */
    const uint32_t st = ddrsc_rd(FX1_DDRSC_STATUS);
    fx1_check((st & FX1_DDRSC_ST_PREINIT) && (st & FX1_DDRSC_ST_MISSION_READY), 0x10, "A: preinit and mission ready");
    fx1_check(ddr3_identify() == DDR3_OK, 0x11, "A: identity");
    expect_eq(ddr3_rd(FX1_DDR3_GLOBAL_CTRL), FX1_DDR3_GLOBAL_CTRL_ENABLE, 0x12, "A: GLOBAL_CTRL.enable");
    fx1_check(ddr3_snap(FX1_DDR3_INIT_STATUS) & FX1_DDR3_INIT_ST_DRAM_INIT, 0x13, "A: DRAM initialized");
    expect_eq(ddr3_rd(FX1_DDR3_TOPO_ACTIVE), DDR3_REF_TOPO, 0x14, "A: active topology = reference");
    for (uint32_t i = 0; i < FX1_DDR3_TIMING_WORDS; ++i)
        expect_eq(ddr3_rd(FX1_DDR3_TIMING_ACTIVE(i)), ddr3_ref_timing[i], 0x15, "A: active timing = reference");
    fx1_check(ddrsc_rd(FX1_DDRSC_QOS_CFG) & 1u, 0x16, "A: port 0 QoS configured by the platform");
    fx1_log("A: platform-initialized DDR3 matches the firmware reference image");

    /* B */
    fx1_release_secondaries();
    expect_eq(memory_pass(0), 0, 0x20, "B: hart 0 memory pass");
    while (!hart1_done) ddr3_sleep_us(10);
    fx1_acquire();
    expect_eq(hart1_errors, 0, 0x21, "B: hart 1 memory pass");
    fx1_log("B: both harts, DDR code/data/stacks and test traffic");

    /* C: route DDR interrupts */
    fx1_write32(CE_ADDR, 0xC0FFEE00u);
    fx1_write32(UE_ADDR, 0xDEADBEEFu);
    ddr3_wr(FX1_DDR3_CE_THRESHOLD, 1u);
    ddr3_wr(FX1_DDR3_IRQ_CLEAR, 0xFFFFFFFFu);
    ddr3_wr(FX1_DDR3_IRQ_MASK, (1u << 0) | (1u << 1));
    fx1_plic_set_priority(FX1_IRQ_DDR, 1);
    fx1_plic_set_threshold(FX1_PLIC_CONTEXT_HART_M(0), 0);
    fx1_plic_enable(FX1_PLIC_CONTEXT_HART_M(0), FX1_IRQ_DDR, 1);
    fx1_irq_enable(FX1_MIE_MEIE);
    fx1_global_irq_enable();
    wait_injection(FAULT_CE_US, 0x30);
    expect_eq(fx1_read32(CE_ADDR), 0xC0FFEE00u, 0x31, "C: corrected data");
    expect_eq(ddr3_snap(FX1_DDR3_CE_COUNT), 1, 0x32, "C: CE_COUNT 1");
    ddr3_sleep_us(5);
    fx1_check(irq_seen & 1u, 0x33, "C: IRQ_RAW[0] through PLIC");
    fx1_log("C: correctable error from the host schedule");

    /* D */
    wait_injection(FAULT_UE_US, 0x40);
    uint32_t v = 0;
    expect_eq(ddr3_probe_read32(UE_ADDR, &v), 5u, 0x41, "D: uncorrectable -> load access fault");
    expect_eq(ddr3_snap(FX1_DDR3_UE_COUNT), 1, 0x42, "D: UE_COUNT 1");
    ddr3_sleep_us(5);
    fx1_check(irq_seen & 2u, 0x43, "D: IRQ_RAW[1] through PLIC");
    fx1_log("D: uncorrectable error -> access fault");

    /* E */
    fx1_global_irq_disable();
    const uint32_t ps = ddr3_power_request(FX1_DDR3_PSTATE_SELF_REFRESH);
    fx1_check(ps & FX1_DDRSC_PWR_ACCEPTED, 0x50, "E: self-refresh accepted while running from DDR");
    const uint32_t count = ddrsc_rd(FX1_DDRSC_PWR_COUNT);
    fx1_check(FX1_DDRSC_PWR_ENTRIES(count) >= 1 && FX1_DDRSC_PWR_AUTO_WAKES(count) >= 1, 0x51,
              "E: entered, then woken by the instruction fetch");
    expect_eq(FX1_DDRSC_PWR_RESIDENT(ddrsc_rd(FX1_DDRSC_PWR_STATUS)), FX1_DDR3_PSTATE_ACTIVE, 0x52, "E: active");
    fx1_log("E: self-refresh and automatic wake while executing from DDR");
    return 0;
}
