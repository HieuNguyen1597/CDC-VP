#include "regression_bench.h"
int sc_main(int argc,char** argv) {
    if(argc<2) return 2;
    const unsigned seed=argc>2?std::stoul(argv[2]):0xF003;
    std::cout << "SystemC=" << sc_core::sc_version() << " seed=" << seed << '\n';
    Regression bench{"bench",argv[1],seed}; sc_core::sc_start();
    return bench.errors?1:0;
}
