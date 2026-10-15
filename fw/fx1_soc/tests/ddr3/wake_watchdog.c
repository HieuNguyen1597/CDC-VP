/*
 * A failed self-refresh exit with no recovery agent (review S5-R1; --ddr-boot firmware).
 * BootROM image. Self-refresh, POWER_TIMEOUT = 1 cycle (legal), then a DDR read: the wake
 * times out, every automatic wake is denied and the read stays held for ever. The
 * simulated time must keep advancing so that the --timeout-ms budget ends the run with
 * RESULT TIMEOUT (exit status 2); before the fix the kernel looped at one instant.
 */
#include "ddr3_fw.h"

int main(void)
{
    ddr3_install_probe_handler();
    fx1_check(ddr3_bringup(2000u) == DDR3_OK, 0x7A, "bring-up");
    fx1_check(ddr3_power_request(FX1_DDR3_PSTATE_SELF_REFRESH) & FX1_DDRSC_PWR_ACCEPTED, 0x7B,
              "self-refresh entered");
    ddr3_wr(FX1_DDR3_POWER_TIMEOUT, 1u);
    fx1_log("wake watchdog: DDR read with a failing wake, no recovery agent");
    uint32_t v = 0;
    (void)ddr3_probe_read32(0x80100000u, &v);
    fx1_log("the held read returned"); /* not reached */
    return 0;
}
