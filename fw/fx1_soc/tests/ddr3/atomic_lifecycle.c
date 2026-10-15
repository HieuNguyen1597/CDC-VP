/*
 * CPU LR/SC across DDR3 lifecycle events (review S5-R2; --ddr-boot firmware).
 * BootROM image. Every event that changes what a DDR address holds or designates must
 * void a reservation taken before it, even though no bus write touched the granule:
 *
 *  A  control: LR/SC with no event in between succeeds
 *  B  cold reset (DDR_SYSCTRL) + full initialization: the scrub zeroes the word
 *  C  coordinated core reset + recommit of the surviving shadows: initialization again
 *  D  CSR-driven reinitialization: global quiesce + TOPO_COMMIT (no reset at all)
 *  E  region commit (same map): the address may designate other storage
 *
 * In B-E the SC must fail and must not write. Fail codes: 0x4x.
 */
#include "ddr3_fw.h"

#define WORD 0x80100000u

static uint32_t lr(uint32_t addr)
{
    uint32_t v;
    __asm__ volatile("lr.w %0, (%1)" : "=r"(v) : "r"(addr) : "memory");
    return v;
}

static uint32_t sc(uint32_t addr, uint32_t value)
{
    uint32_t failed;
    __asm__ volatile("sc.w %0, %2, (%1)" : "=&r"(failed) : "r"(addr), "r"(value) : "memory");
    return failed;
}

static void bringup(uint32_t code)
{
    const ddr3_status_t s = ddr3_bringup(2000u);
    if (s != DDR3_OK) ddr3_log_status("bring-up", s);
    fx1_check(s == DDR3_OK, code, "DDR3 ready");
}

static void wait_ready(uint32_t code)
{
    const ddr3_status_t s = ddr3_wait_ready(2000u);
    if (s != DDR3_OK) ddr3_log_status("readiness", s);
    fx1_check(s == DDR3_OK, code, "DDR3 ready again");
}

/* The SC after the event fails and leaves `expect` in memory. */
static void expect_sc_fails(uint32_t expect, uint32_t code, const char* what)
{
    const uint32_t rc = sc(WORD, 0xBADBAD00u);
    const uint32_t now = fx1_read32(WORD);
    if (rc == 0 || now != expect) {
        fx1_log_hex("  SC result (nonzero expected)", rc);
        fx1_log_hex("  word", now);
    }
    fx1_check(rc != 0, code, what);
    fx1_check(now == expect, code + 1, "the failed SC wrote nothing");
    fx1_log(what);
}

int main(void)
{
    ddr3_install_probe_handler();
    fx1_log("CPU reservations across DDR3 lifecycle events");
    bringup(0x40);

    /* A */
    fx1_write32(WORD, 10);
    fx1_check(sc(WORD, lr(WORD) + 1) == 0 && fx1_read32(WORD) == 11, 0x41, "A: LR/SC without an event succeeds");

    /* B */
    fx1_write32(WORD, 0x12345678u);
    (void)lr(WORD);
    const uint32_t rs = ddr3_reset(FX1_DDRSC_RST_COLD);
    fx1_check(rs & FX1_DDRSC_RST_DONE, 0x42, "B: cold reset done");
    bringup(0x43);
    expect_sc_fails(0, 0x44, "B: cold reset + initialization voids the reservation");

    /* C */
    fx1_write32(WORD, 0x11111111u);
    (void)lr(WORD);
    fx1_check(FX1_DDRSC_RST_CORE_COUNT(ddr3_reset(FX1_DDRSC_RST_CORE)) == 1, 0x46, "C: core reset done");
    fx1_check(ddr3_commit_all() == DDR3_OK, 0x47, "C: recommit of the surviving shadows");
    wait_ready(0x48);
    expect_sc_fails(0, 0x49, "C: core reset + reinitialization voids the reservation");

    /* D */
    fx1_write32(WORD, 0x22222222u);
    (void)lr(WORD);
    ddr3_wr(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_REQUEST);
    while (!(ddr3_rd(FX1_DDR3_QUIESCE_STATUS) & FX1_DDR3_QUIESCE_ST_COMPLETE)) ddr3_sleep_us(1);
    fx1_check(ddr3_probe_write32(FX1_DDR3_CSR_BASE + FX1_DDR3_TOPO_COMMIT, 1) == 0, 0x4B, "D: TOPO_COMMIT accepted");
    ddr3_wr(FX1_DDR3_QUIESCE_CTRL, FX1_DDR3_QUIESCE_CTRL_RELEASE);
    wait_ready(0x4C);
    expect_sc_fails(0, 0x4D, "D: CSR-driven reinitialization voids the reservation");

    /* E */
    fx1_write32(WORD, 0x33333333u);
    (void)lr(WORD);
    ddr3_wr(FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 0, 0)), FX1_DDR_BASE);
    ddr3_wr(FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 0, 1)), FX1_DDR_SIZE);
    fx1_check(ddr3_probe_write32(FX1_DDR3_CSR_BASE + FX1_DDR3_FE_REGION_COMMIT, 1) == 0, 0x4F, "E: region commit");
    expect_sc_fails(0x33333333u, 0x50, "E: a region commit voids the reservation, data kept");
    return 0;
}
