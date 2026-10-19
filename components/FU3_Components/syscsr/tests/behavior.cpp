// SYSCSR nối thẳng (không qua bus): reset value, mask, lane/PSTRB, status, sys_ctrl, reset_n, lỗi truy cập.
#include "bench.h"

int sc_main(int, char**) {
    Bench bench("Bench", false);
    sc_core::sc_start();
    std::cout << "SYSCSR behavior: " << bench.checks << " checks, " << bench.errors << " failures\n";
    return bench.errors ? 1 : 0;
}
