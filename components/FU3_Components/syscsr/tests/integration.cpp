// Cùng bộ kiểm tra nhưng đi qua bus FU3 thật: IBEX -> SYSBUS -> PERIBUS_0 -> A2P -> SYSCSR @ 0x4401_0000.
#define FU3_SYSCSR_WITH_BUS
#include "bench.h"

int sc_main(int, char**) {
    Bench bench("Bench", true);
    sc_core::sc_start();
    std::cout << "SYSCSR integration (FU3 bus): " << bench.checks << " checks, " << bench.errors << " failures\n";
    return bench.errors ? 1 : 0;
}
