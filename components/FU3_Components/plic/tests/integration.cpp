// Cùng bộ kiểm tra nhưng đi qua bus FU3 thật: CVA6 -> SYSBUS -> PERIBUS_1 -> A2P -> AP_PLIC @ 0x4010_0000.
#define FU3_PLIC_WITH_BUS
#include "bench.h"

int sc_main(int, char**) {
    Bench bench("Bench", true);
    sc_core::sc_start();
    std::cout << "PLIC integration (FU3 bus): " << bench.checks << " checks, " << bench.errors << " failures\n";
    return bench.errors ? 1 : 0;
}
