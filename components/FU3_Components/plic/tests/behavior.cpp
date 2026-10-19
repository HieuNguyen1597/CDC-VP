// PLIC nối thẳng (không qua bus): thanh ghi, gateway level/edge, claim/complete, priority/threshold/IE, reset, lỗi truy cập.
#include "bench.h"

int sc_main(int, char**) {
    Bench bench("Bench", false);
    sc_core::sc_start();
    std::cout << "PLIC behavior: " << bench.checks << " checks, " << bench.errors << " failures\n";
    return bench.errors ? 1 : 0;
}
