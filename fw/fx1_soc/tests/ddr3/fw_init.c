/*
 * DDR3 firmware-init test (DDR3 VP subtask 5; run with --ddr-boot firmware).
 * BootROM image: code in BootROM, data and stacks in SRAM, no DDR access before
 * readiness. Hart 1 waits in WFI until phase F.
 *
 *  A  identity and reset state: IP_ID, ABI, capability, DDR_SYSCTRL ID and STATUS
 *     (not preinitialized, not ready), INIT_CMD answers SLVERR (DEC-05)
 *  B  commit validation: an illegal CL makes TIMING_COMMIT answer SLVERR with
 *     blocker 0x04 and IRQ_RAW[4]; the corrected image commits; GLOBAL_CTRL.enable;
 *     readiness through DDR_SYSCTRL after the startup ECC scrub
 *  C  memory: word/half/byte accesses, the first and last word of the aperture,
 *     scrubbed memory reads zero, no CE/UE
 *  D  region decode miss: a 256 MiB region map; an access above it is a load access
 *     fault on the CPU and IRQ_RAW[4] reaches hart 0 through PLIC source 8
 *  E  power: self-refresh through DDR_SYSCTRL; a DDR access wakes it (PACTIVE)
 *  F  two harts: both harts run DDR traffic and AMOs on one DDR counter
 *
 * Fail codes: 0x1x A, 0x2x B, 0x3x C, 0x4x D, 0x5x E, 0x6x F.
 */
#include "ddr3_fw.h"
#include "ddr3_ref_1600.h"

#define DDR_TEST     0x80200000u
#define HART1_AREA   0x80300000u
#define SHARED_COUNT 0x80380000u
#define AMO_ROUNDS   100u
#define DDR_LAST     (FX1_DDR_BASE + FX1_DDR_SIZE - 4u)

static volatile uint32_t irq_count, irq_seen;
static volatile uint32_t hart1_done, hart1_errors;

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
            ++irq_count;
            ddr3_wr(FX1_DDR3_IRQ_CLEAR, pending); /* at the device before the PLIC complete */
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

static uint32_t pattern(uint32_t address) { return address * 2654435761u ^ 0xA5C3u; }

static void phase_a(void)
{
    fx1_check(ddr3_identify() == DDR3_OK, 0x10, "A: IP_ID, ABI 1.x, ECC and 2 ranks, DDR_SYSCTRL ID");
    const uint32_t st = ddrsc_rd(FX1_DDRSC_STATUS);
    fx1_check(!(st & FX1_DDRSC_ST_PREINIT), 0x11, "A: firmware boot (STATUS.preinit = 0)");
    fx1_check(!(st & (FX1_DDRSC_ST_MISSION_READY | FX1_DDRSC_ST_INIT_COMPLETE)), 0x12, "A: not ready at reset");
    expect_eq(ddr3_rd(FX1_DDR3_GLOBAL_CTRL), 0, 0x13, "A: GLOBAL_CTRL reset value");
    expect_eq(ddr3_probe_write32(FX1_DDR3_CSR_BASE + FX1_DDR3_INIT_CMD, 1), 7u, 0x14,
              "A: INIT_CMD answers SLVERR (store access fault)");
    fx1_log("A: identity, capability and reset state");
}

static void phase_b(void)
{
    ddr3_qos_default();
    fx1_check(ddr3_program() == DDR3_OK, 0x20, "B: shadows and INIT_RW words written");
    fx1_check(ddr3_probe_write32(FX1_DDR3_CSR_BASE + FX1_DDR3_TOPO_COMMIT, 1) == 0, 0x21, "B: topology commit");
    ddr3_wr(FX1_DDR3_TIMING_SHADOW(FX1_DDR3_T_CL), 4u); /* CL outside 5..11 */
    expect_eq(ddr3_probe_write32(FX1_DDR3_CSR_BASE + FX1_DDR3_TIMING_COMMIT, 1), 7u, 0x22,
              "B: illegal timing commit answers SLVERR");
    const uint32_t cs = ddr3_rd(FX1_DDR3_TIMING_COMMIT_STATUS);
    fx1_check(cs & FX1_DDR3_COMMIT_ST_ERROR, 0x23, "B: TIMING_COMMIT_STATUS error");
    expect_eq(FX1_DDR3_COMMIT_ST_IMM_BLOCKER(cs), FX1_DDR3_BLK_TIMING, 0x24, "B: blocker 0x04 (timing illegal)");
    fx1_check(ddr3_rd(FX1_DDR3_IRQ_RAW) & (1u << 4), 0x25, "B: IRQ_RAW[4] commit reject");
    ddr3_wr(FX1_DDR3_IRQ_CLEAR, 0xFFFFFFFFu);
    ddr3_wr(FX1_DDR3_COMMAND_ERROR, 0xFFFFFFFFu);
    ddr3_wr(FX1_DDR3_TIMING_SHADOW(FX1_DDR3_T_CL), ddr3_ref_timing[FX1_DDR3_T_CL]);
    fx1_check(ddr3_commit_all() == DDR3_OK, 0x26, "B: corrected images commit");
    ddr3_wr(FX1_DDR3_GLOBAL_CTRL, FX1_DDR3_GLOBAL_CTRL_ENABLE);
    const ddr3_status_t s = ddr3_wait_ready(2000u);
    if (s != DDR3_OK) ddr3_log_status("B: readiness", s);
    fx1_check(s == DDR3_OK, 0x27, "B: mission ready (DDR_SYSCTRL STATUS)");
    fx1_check(ddr3_snap(FX1_DDR3_INIT_STATUS) & FX1_DDR3_INIT_ST_DRAM_INIT, 0x28, "B: INIT_STATUS DRAM initialized");
    fx1_check(!(ddr3_rd(FX1_DDR3_INIT_STATUS) & FX1_DDR3_INIT_ST_ADM_BLOCK), 0x29, "B: admission open");
    fx1_log_hex("B: commit reject, recommit, ready after us", ddr3_last.elapsed_us);
}

static void phase_c(void)
{
    volatile uint32_t* w = (volatile uint32_t*)DDR_TEST;
    for (uint32_t i = 0; i < 256; ++i) w[i] = pattern(DDR_TEST + 4 * i);
    for (uint32_t i = 0; i < 256; ++i) expect_eq(w[i], pattern(DDR_TEST + 4 * i), 0x30, "C: word pattern");
    volatile uint8_t* b = (volatile uint8_t*)(DDR_TEST + 0x1001u);
    volatile uint16_t* h = (volatile uint16_t*)(DDR_TEST + 0x1006u);
    b[0] = 0x5A;
    h[0] = 0xBEEF;
    expect_eq(fx1_read32(DDR_TEST + 0x1000u), 0x00005A00u, 0x31, "C: byte write merges into its word");
    expect_eq(fx1_read32(DDR_TEST + 0x1004u), 0xBEEF0000u, 0x32, "C: halfword write merges into its word");
    fx1_write32(FX1_DDR_BASE, 0x600DF00Du);
    fx1_write32(DDR_LAST, 0x1A57F00Du);
    expect_eq(fx1_read32(FX1_DDR_BASE), 0x600DF00Du, 0x33, "C: first word of the aperture");
    expect_eq(fx1_read32(DDR_LAST), 0x1A57F00Du, 0x34, "C: last word of the aperture");
    expect_eq(fx1_read32(0x88000000u), 0, 0x35, "C: scrubbed memory reads zero");
    expect_eq(ddr3_snap(FX1_DDR3_CE_COUNT), 0, 0x36, "C: no corrected error");
    expect_eq(ddr3_snap(FX1_DDR3_UE_COUNT), 0, 0x37, "C: no uncorrectable error");
    fx1_log("C: word/half/byte accesses, aperture ends, scrubbed zero, no ECC event");
}

static void set_region(uint32_t size)
{
    ddr3_wr(FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 0, 0)), FX1_DDR_BASE);
    ddr3_wr(FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 0, 1)), size);
    fx1_check(ddr3_probe_write32(FX1_DDR3_CSR_BASE + FX1_DDR3_FE_REGION_COMMIT, 1) == 0, 0x40, "D: region commit");
    fx1_check(ddr3_rd(FX1_DDR3_FE_REGION_STATUS) & FX1_DDR3_FE_REGION_ST_VALID, 0x41, "D: region image installed");
}

static void phase_d(void)
{
    fx1_plic_set_priority(FX1_IRQ_DDR, 1);
    fx1_plic_set_threshold(FX1_PLIC_CONTEXT_HART_M(0), 0);
    fx1_plic_enable(FX1_PLIC_CONTEXT_HART_M(0), FX1_IRQ_DDR, 1);
    ddr3_wr(FX1_DDR3_IRQ_MASK, 1u << 4);
    fx1_irq_enable(FX1_MIE_MEIE);
    fx1_global_irq_enable();

    set_region(0x10000000u); /* 256 MiB */
    uint32_t v = 0;
    expect_eq(ddr3_probe_read32(0x90000000u, &v), 5u, 0x42, "D: access above the region: load access fault");
    expect_eq(v, 0, 0x43, "D: no data from a decode miss");
    ddr3_sleep_us(10);
    fx1_check(irq_count >= 1 && (irq_seen & (1u << 4)), 0x44, "D: IRQ_RAW[4] through PLIC source 8");
    expect_eq(ddr3_probe_read32(0x8FFFFFFCu, &v), 0, 0x45, "D: the last word of the region is accessible");
    set_region(FX1_DDR_SIZE);
    expect_eq(ddr3_probe_read32(DDR_LAST, &v), 0, 0x46, "D: the full aperture again");
    expect_eq(v, 0x1A57F00Du, 0x47, "D: data kept across region commits");
    fx1_global_irq_disable();
    fx1_log("D: region decode miss -> load access fault and PLIC interrupt");
}

static void phase_e(void)
{
    uint32_t st = ddr3_power_request(FX1_DDR3_PSTATE_SELF_REFRESH);
    fx1_check(st & FX1_DDRSC_PWR_ACCEPTED, 0x50, "E: self-refresh accepted");
    expect_eq(FX1_DDRSC_PWR_RESIDENT(st), FX1_DDR3_PSTATE_SELF_REFRESH, 0x51, "E: resident self-refresh");
    expect_eq(fx1_read32(FX1_DDR_BASE), 0x600DF00Du, 0x52, "E: an access wakes the DDR, data retained");
    const uint32_t count = ddrsc_rd(FX1_DDRSC_PWR_COUNT);
    expect_eq(FX1_DDRSC_PWR_ENTRIES(count), 1, 0x53, "E: one low-power entry");
    expect_eq(FX1_DDRSC_PWR_AUTO_WAKES(count), 1, 0x54, "E: one automatic wake (PACTIVE)");
    expect_eq(FX1_DDRSC_PWR_RESIDENT(ddrsc_rd(FX1_DDRSC_PWR_STATUS)), FX1_DDR3_PSTATE_ACTIVE, 0x55, "E: active");
    fx1_log("E: self-refresh through DDR_SYSCTRL, automatic wake on access");
}

static void amo_rounds(void)
{
    for (uint32_t i = 0; i < AMO_ROUNDS; ++i)
        __atomic_fetch_add((volatile uint32_t*)SHARED_COUNT, 1u, __ATOMIC_RELAXED);
}

void secondary_main(uint32_t hart)
{
    volatile uint32_t* w = (volatile uint32_t*)HART1_AREA;
    for (uint32_t i = 0; i < 512; ++i) w[i] = pattern(HART1_AREA + 4 * i) + hart;
    amo_rounds();
    uint32_t errors = 0;
    for (uint32_t i = 0; i < 512; ++i) errors += w[i] != pattern(HART1_AREA + 4 * i) + hart;
    hart1_errors = errors;
    fx1_release();
    hart1_done = 1;
    for (;;) fx1_wfi();
}

static void phase_f(void)
{
    fx1_write32(SHARED_COUNT, 0);
    fx1_release_secondaries();
    volatile uint32_t* w = (volatile uint32_t*)DDR_TEST;
    for (uint32_t i = 0; i < 512; ++i) w[i] = ~pattern(DDR_TEST + 4 * i);
    amo_rounds();
    for (uint32_t i = 0; i < 512; ++i) expect_eq(w[i], ~pattern(DDR_TEST + 4 * i), 0x60, "F: hart 0 data");
    while (!hart1_done) ddr3_sleep_us(5);
    fx1_acquire();
    expect_eq(hart1_errors, 0, 0x61, "F: hart 1 data");
    expect_eq(fx1_read32(SHARED_COUNT), 2u * AMO_ROUNDS, 0x62, "F: AMO counter in DDR from both harts");
    fx1_log("F: two harts on DDR, AMO counter consistent");
}

int main(void)
{
    fx1_set_trap_handler(handler);
    fx1_log("DDR3 firmware init (BootROM/SRAM), FX1 VP");
    phase_a();
    phase_b();
    phase_c();
    phase_d();
    phase_e();
    phase_f();
    expect_eq(ddr3_rd(FX1_DDR3_FATAL_STATUS), 0, 0x70, "no fatal event");
    expect_eq(ddr3_snap(FX1_DDR3_UE_COUNT), 0, 0x71, "no uncorrectable error");
    return 0;
}
