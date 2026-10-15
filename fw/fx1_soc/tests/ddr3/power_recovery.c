/*
 * Failed self-refresh exit seen by the CPU, and its recovery (review S5-R1;
 * --ddr-boot firmware). BootROM image.
 *
 * Hart 0 brings the DDR3 up, enters self-refresh through DDR_SYSCTRL, sets POWER_TIMEOUT
 * to 1 cycle (a legal value) and reads DDR. The automatic wake times out after the cutoff:
 * the controller requires recovery and denies every further ACTIVE request, so the read
 * stays held. DDR_SYSCTRL keeps retrying with a back-off (PWR_STATUS.AUTO_WAKE_DENIED)
 * and the kernel keeps advancing. Hart 1, released before the read, observes the denial
 * and POWER_STATUS[5] (recovery required) and issues the cold reset, which aborts the held
 * read (load access fault on hart 0). Hart 0 then initializes the DDR3 again.
 *
 * Fail codes: 0x7x.
 */
#include "ddr3_fw.h"

#define WORD 0x80100000u

static volatile uint32_t hart1_power_status, hart1_power_flags, hart1_reset_status, hart1_done;

void secondary_main(uint32_t hart)
{
    (void)hart;
    const uint64_t limit = fx1_mtime() + ddr3_ms_to_ticks(50);
    uint32_t ps = 0;
    while (!((ps = ddrsc_rd(FX1_DDRSC_PWR_STATUS)) & FX1_DDRSC_PWR_AUTO_WAKE_DENIED)) {
        if (fx1_mtime() > limit) fx1_fail(0x7E);
        ddr3_sleep_us(5u);
    }
    hart1_power_status = ps;
    hart1_power_flags = ddr3_snap(FX1_DDR3_POWER_STATUS);
    hart1_reset_status = ddr3_reset(FX1_DDRSC_RST_COLD);
    fx1_release();
    hart1_done = 1;
    for (;;) fx1_wfi();
}

int main(void)
{
    ddr3_install_probe_handler();
    fx1_log("DDR3 wake failure and recovery on the CPU");
    ddr3_status_t s = ddr3_bringup(2000u);
    fx1_check(s == DDR3_OK, 0x70, "bring-up");
    fx1_write32(WORD, 0xABCD0001u);
    const uint32_t ps = ddr3_power_request(FX1_DDR3_PSTATE_SELF_REFRESH);
    fx1_check(ps & FX1_DDRSC_PWR_ACCEPTED, 0x71, "self-refresh entered");
    ddr3_wr(FX1_DDR3_POWER_TIMEOUT, 1u);
    fx1_release_secondaries();

    uint32_t v = 0;
    const uint64_t t0 = fx1_mtime();
    const uint32_t cause = ddr3_probe_read32(WORD, &v);
    fx1_log_hex("held read ended after us", (uint32_t)((fx1_mtime() - t0) / DDR3_TICKS_PER_US));
    fx1_check(cause == 5u, 0x72, "the held read is aborted by the cold reset (load access fault)");
    while (!hart1_done) ddr3_sleep_us(5);
    fx1_acquire();
    fx1_check(hart1_power_status & FX1_DDRSC_PWR_PACTIVE, 0x73, "PACTIVE high while the wake is denied");
    fx1_check(FX1_DDRSC_PWR_RESIDENT(hart1_power_status) == FX1_DDR3_PSTATE_SELF_REFRESH, 0x74,
              "still resident in self-refresh");
    fx1_check(hart1_power_flags & FX1_DDR3_POWER_ST_FATAL_RECOVERY, 0x75, "POWER_STATUS[5] recovery required");
    fx1_check(hart1_reset_status & FX1_DDRSC_RST_DONE, 0x76, "hart 1's cold reset done");
    fx1_check(!(ddrsc_rd(FX1_DDRSC_PWR_STATUS) & FX1_DDRSC_PWR_AUTO_WAKE_DENIED), 0x77, "denial cleared by the reset");
    fx1_log("wake timeout -> automatic wake denied, retried with back-off -> cold reset by hart 1");

    s = ddr3_bringup(2000u);
    fx1_check(s == DDR3_OK, 0x78, "re-initialization");
    fx1_write32(WORD, 0x600DF00Du);
    fx1_check(fx1_read32(WORD) == 0x600DF00Du, 0x79, "DDR usable again");
    fx1_log("re-initialized after the failed wake");
    return 0;
}
