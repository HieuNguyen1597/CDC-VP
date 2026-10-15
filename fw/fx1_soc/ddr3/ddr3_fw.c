/* DDR3 bring-up and test helpers (ddr3_fw.h). */
#include "ddr3_fw.h"

#include "ddr3_ref_1600.h"

ddr3_report_t ddr3_last;

/* Per-hart fault slot of ddr3_probe.S. */
volatile uint32_t ddr3_probe_cause[FX1_NUM_HARTS];
extern const char ddr3_probe_load_insn[], ddr3_probe_store_insn[];

int ddr3_probe_trap(uint32_t mcause, uint32_t mepc, uint32_t* resume)
{
    if (mcause & FX1_MCAUSE_INTERRUPT) return 0;
    if (mepc != (uint32_t)(uintptr_t)ddr3_probe_load_insn && mepc != (uint32_t)(uintptr_t)ddr3_probe_store_insn)
        return 0;
    ddr3_probe_cause[fx1_hartid()] = mcause;
    *resume = mepc + 4u; /* the probed access is one 4-byte instruction */
    return 1;
}

static uint32_t probe_only_handler(uint32_t mcause, uint32_t mepc, uint32_t mtval)
{
    uint32_t resume;
    if (ddr3_probe_trap(mcause, mepc, &resume)) return resume;
    fx1_log_hex("unexpected trap, mcause", mcause);
    fx1_log_hex("  mepc", mepc);
    fx1_log_hex("  mtval", mtval);
    fx1_fail(0xE000u | (mcause & 0xFFu));
}

void ddr3_install_probe_handler(void) { fx1_set_trap_handler(probe_only_handler); }

uint32_t ddr3_snap(uint32_t offset)
{
    ddr3_wr(FX1_DDR3_SNAPSHOT_CMD, FX1_DDR3_SNAPSHOT_CMD_CAPTURE);
    return ddr3_rd(offset);
}

/* ---- time ------------------------------------------------------------------ */
void ddr3_sleep_until(uint64_t ticks)
{
    const uint32_t hart = fx1_hartid();
    const uint32_t saved = fx1_irq_save(); /* WFI wakes on MTIP without taking the trap */
    fx1_irq_enable(FX1_MIE_MTIE);
    while (fx1_mtime() < ticks) {
        fx1_set_mtimecmp(hart, ticks);
        if (fx1_mtime() >= ticks) break;
        fx1_wfi();
    }
    fx1_set_mtimecmp(hart, UINT64_MAX);
    fx1_irq_disable(FX1_MIE_MTIE);
    fx1_irq_restore(saved);
}

void ddr3_sleep_us(uint32_t us) { ddr3_sleep_until(fx1_mtime() + (uint64_t)us * DDR3_TICKS_PER_US); }

/* ---- bring-up ---------------------------------------------------------------- */
static ddr3_status_t fail_at(ddr3_status_t status, uint32_t offset, uint32_t value)
{
    ddr3_last.offset = offset;
    ddr3_last.value = value;
    return status;
}

ddr3_status_t ddr3_identify(void)
{
    if (ddr3_rd(FX1_DDR3_IP_ID) != 0x44445233u) return fail_at(DDR3_ERR_IDENTITY, FX1_DDR3_IP_ID, ddr3_rd(FX1_DDR3_IP_ID));
    const uint32_t abi = ddr3_rd(FX1_DDR3_ABI_VERSION);
    if ((abi >> 16) != 1u) return fail_at(DDR3_ERR_IDENTITY, FX1_DDR3_ABI_VERSION, abi);
    const uint32_t feat = ddr3_rd(FX1_DDR3_CAP_FEATURE);
    if (!(feat & FX1_DDR3_CAP_FEATURE_ECC) || FX1_DDR3_CAP_FEATURE_RANKS(feat) < DDR3_REF_RANKS)
        return fail_at(DDR3_ERR_IDENTITY, FX1_DDR3_CAP_FEATURE, feat);
    if (ddrsc_rd(FX1_DDRSC_ID) != FX1_DDRSC_ID_VALUE)
        return fail_at(DDR3_ERR_IDENTITY, FX1_DDRSC_ID, ddrsc_rd(FX1_DDRSC_ID));
    return DDR3_OK;
}

void ddr3_qos_default(void)
{
    ddrsc_wr(FX1_DDRSC_QOS_LUT(0), FX1_DDRSC_QOS_LUT_ALL_NORMAL);
    ddrsc_wr(FX1_DDRSC_QOS_CFG, 1u);
}

static int write_ok(uint32_t offset, uint32_t value)
{
    return ddr3_probe_write32(FX1_DDR3_CSR_BASE + offset, value) == 0;
}

ddr3_status_t ddr3_program(void)
{
    /* §4.1 steps 1-3: timing and DFI shadows, every INIT_RW word, topology shadow. */
    for (uint32_t i = 0; i < FX1_DDR3_TIMING_WORDS; ++i)
        if (!write_ok(FX1_DDR3_TIMING_SHADOW(i), ddr3_ref_timing[i]))
            return fail_at(DDR3_ERR_PROGRAM, FX1_DDR3_TIMING_SHADOW(i), ddr3_ref_timing[i]);
    const uint32_t words[][2] = {
        {FX1_DDR3_DFI_WR_ENABLE_DELAY, DDR3_REF_DFI_WR_ENABLE_DELAY},
        {FX1_DDR3_DFI_RD_ENABLE_DELAY, DDR3_REF_DFI_RD_ENABLE_DELAY},
        {FX1_DDR3_PHY_MAX_READ_RETURN, DDR3_REF_PHY_MAX_READ_RETURN},
        {FX1_DDR3_DFI_ASSEMBLY_MARGIN, DDR3_REF_DFI_ASSEMBLY_MARGIN},
        {FX1_DDR3_INIT_RESET_RELEASE_WAIT, DDR3_REF_INIT_RESET_RELEASE},
        {FX1_DDR3_INIT_CKE_LOW_WAIT, DDR3_REF_INIT_CKE_LOW},
        {FX1_DDR3_INIT_CKE_HIGH_WAIT, DDR3_REF_INIT_CKE_HIGH},
        {FX1_DDR3_INIT_DLL_WAIT, DDR3_REF_INIT_DLL},
        {FX1_DDR3_INIT_ZQ_WAIT, DDR3_REF_INIT_ZQ},
        {FX1_DDR3_TOPO_SHADOW, DDR3_REF_TOPO},
    };
    for (uint32_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i)
        if (!write_ok(words[i][0], words[i][1])) return fail_at(DDR3_ERR_PROGRAM, words[i][0], words[i][1]);
    return DDR3_OK;
}

ddr3_status_t ddr3_commit_all(void)
{
    /* §4.1 steps 3-4: topology, then timing, then DFI (checked against the active timing). */
    static const uint32_t commits[][2] = {
        {FX1_DDR3_TOPO_COMMIT, FX1_DDR3_TOPO_COMMIT_STATUS},
        {FX1_DDR3_TIMING_COMMIT, FX1_DDR3_TIMING_COMMIT_STATUS},
        {FX1_DDR3_DFI_TIMING_COMMIT, FX1_DDR3_DFI_TIMING_STATUS},
    };
    for (uint32_t i = 0; i < 3; ++i)
        if (!write_ok(commits[i][0], 1u)) return fail_at(DDR3_ERR_COMMIT, commits[i][0], ddr3_rd(commits[i][1]));
    return DDR3_OK;
}

ddr3_status_t ddr3_wait_ready(uint32_t timeout_ms)
{
    const uint64_t start = fx1_mtime();
    const uint64_t end = start + ddr3_ms_to_ticks(timeout_ms);
    ddr3_status_t status = DDR3_OK;
    for (;;) {
        if (ddrsc_rd(FX1_DDRSC_STATUS) & FX1_DDRSC_ST_MISSION_READY) break;
        const uint32_t fatal = ddr3_rd(FX1_DDR3_FATAL_STATUS);
        if (fatal) {
            status = fail_at(DDR3_ERR_FATAL, FX1_DDR3_FATAL_STATUS, fatal);
            break;
        }
        const uint32_t init = ddr3_snap(FX1_DDR3_INIT_STATUS);
        if (init & FX1_DDR3_INIT_ST_FAILED) {
            status = fail_at(DDR3_ERR_INIT_FAILED, FX1_DDR3_INIT_STATUS, init);
            break;
        }
        if (fx1_mtime() >= end) {
            status = fail_at(DDR3_ERR_TIMEOUT, FX1_DDR3_INIT_STATUS, init);
            break;
        }
        ddr3_sleep_us(1000u); /* poll every millisecond: the scrub takes hundreds */
    }
    ddr3_last.elapsed_us = (uint32_t)((fx1_mtime() - start) / DDR3_TICKS_PER_US);
    return status;
}

ddr3_status_t ddr3_bringup(uint32_t timeout_ms)
{
    ddr3_last.offset = ddr3_last.value = ddr3_last.elapsed_us = 0;
    ddr3_status_t s = ddr3_identify();
    if (s != DDR3_OK) return s;
    ddr3_qos_default();
    if ((s = ddr3_program()) != DDR3_OK) return s;
    if ((s = ddr3_commit_all()) != DDR3_OK) return s;
    /* §4.1 step 5: accepted memory transactions wait until enable and readiness. */
    if (!write_ok(FX1_DDR3_GLOBAL_CTRL, FX1_DDR3_GLOBAL_CTRL_ENABLE))
        return fail_at(DDR3_ERR_PROGRAM, FX1_DDR3_GLOBAL_CTRL, FX1_DDR3_GLOBAL_CTRL_ENABLE);
    return ddr3_wait_ready(timeout_ms);
}

void ddr3_log_status(const char* what, ddr3_status_t status)
{
    static const char* const names[] = {"ok", "identity mismatch", "register write rejected", "commit rejected",
                                        "readiness timeout", "initialization failed", "fatal event"};
    fx1_log(what);
    fx1_log((unsigned)status < sizeof(names) / sizeof(names[0]) ? names[status] : "?");
    fx1_log_hex("  register", ddr3_last.offset);
    fx1_log_hex("  status  ", ddr3_last.value);
    fx1_log_hex("  waited us", ddr3_last.elapsed_us);
}

/* ---- DDR_SYSCTRL sequences ---------------------------------------------------- */
uint32_t ddr3_power_request(uint32_t pstate)
{
    ddrsc_wr(FX1_DDRSC_PWR_REQ, FX1_DDRSC_PWR_REQ_VAL(pstate));
    uint32_t st;
    while ((st = ddrsc_rd(FX1_DDRSC_PWR_STATUS)) & FX1_DDRSC_PWR_BUSY) ddr3_sleep_us(1u);
    return st;
}

uint32_t ddr3_reset(uint32_t which)
{
    ddrsc_wr(FX1_DDRSC_RST_CTRL, which);
    uint32_t st;
    while ((st = ddrsc_rd(FX1_DDRSC_RST_STATUS)) & FX1_DDRSC_RST_BUSY) ddr3_sleep_us(1u);
    return st;
}
