/*
 * Preinit boot order seen from the CPU (DDR3 VP plan §5.3: scrub -> preload ->
 * CPU execution). BootROM image run with the default --ddr-boot preinit and
 * --load PRELOAD_ADDR:<text file>. The hart's first instruction must execute only
 * after the platform finished the DDR3 initialization, the ECC startup scrub of
 * 4 GiB (about 670 ms) and the preload: a BootROM entry would otherwise run during
 * the platform's CSR sequence.
 */
#include "ddr3_fw.h"

#define PRELOAD_ADDR 0x80600000u
static const char expected[] = "FX1 DDR3 preload check: scrub -> preload -> CPU\n";

int main(void)
{
    const uint64_t first = fx1_mtime(); /* first instructions of hart 0 */
    const uint32_t st = ddrsc_rd(FX1_DDRSC_STATUS);
    fx1_check((st & FX1_DDRSC_ST_PREINIT) && (st & FX1_DDRSC_ST_MISSION_READY), 0x01,
              "mission ready before the first instruction");
    fx1_check(first >= ddr3_ms_to_ticks(600), 0x02, "the CPU starts after the startup scrub");
    fx1_log_hex("CPU start, ms after power-on", (uint32_t)(first / ddr3_ms_to_ticks(1)));
    const volatile char* p = (const volatile char*)PRELOAD_ADDR;
    for (unsigned i = 0; i + 1 < sizeof(expected); ++i)
        fx1_check(p[i] == expected[i], 0x03, "the --load file is in DDR before the CPU starts");
    fx1_check(fx1_read32(PRELOAD_ADDR + 0x100u) == 0, 0x04, "the rest of DDR is scrubbed zero");
    fx1_log("preinit order: scrub -> preload -> CPU");
    return 0;
}
