// SPDX-License-Identifier: Apache-2.0
// Package consumer smoke test: public headers and libraries only. Binds the controller, a
// platform bridge and the CSR port, checks an identity register, a DECERR through the
// bridge before any region exists, and the SECDED golden vector of the core library.
#include "fx1_ddr3/ddr3_controller.h"
#include "fx1_ddr3/ecc_codec.h"
#include "fx1_ddr3/fx1_ddr3_regs.h"
#include "fx1_ddr3/generic_axi_bridge.h"

#include <tlm_utils/simple_initiator_socket.h>

#include <cstdio>

struct Top : sc_core::sc_module {
    tlm_utils::simple_initiator_socket<Top> csr{"csr"};
    tlm_utils::simple_initiator_socket<Top> cpu{"cpu"};
    fx1::ddr3::Ddr3Controller ddr{"ddr", [] {
                                      auto c = fx1::ddr3::BuildConfig::legacy();
                                      c.n_ports = 1;
                                      return c;
                                  }()};
    fx1::ddr3::GenericAxiBridge bridge{"bridge", 8};
    sc_core::sc_signal<bool> irq{"irq"};
    int rc = 1;
    SC_HAS_PROCESS(Top);
    explicit Top(sc_core::sc_module_name n) : sc_module(n) {
        csr.bind(ddr.csr_socket);
        ddr.irq.bind(irq);
        cpu.bind(bridge.target);
        bridge.initiator.bind(ddr.mem_socket[0]);
        SC_THREAD(run);
    }
    void run() {
        unsigned char b[4]{};
        tlm::tlm_generic_payload tx;
        tx.set_command(tlm::TLM_READ_COMMAND);
        tx.set_address(FX1_DDR3_CAP_WIDTH0);
        tx.set_data_ptr(b);
        tx.set_data_length(4);
        tx.set_streaming_width(4);
        sc_core::sc_time d = sc_core::SC_ZERO_TIME;
        csr->b_transport(tx, d);
        const unsigned v = b[0] | b[1] << 8 | b[2] << 16 | unsigned(b[3]) << 24;
        ddr.qos_set_configured(0, true);
        tlm::tlm_generic_payload m;
        m.set_command(tlm::TLM_READ_COMMAND);
        m.set_address(0x80000000u);
        m.set_data_ptr(b);
        m.set_data_length(4);
        m.set_streaming_width(4);
        cpu->b_transport(m, d);
        rc = (tx.is_response_ok() && v == fx1::ddr3::caps::kCapWidth0Ratio1 &&
              m.get_response_status() == tlm::TLM_ADDRESS_ERROR_RESPONSE &&
              fx1::ddr3::ecc_encode(0x0123456789ABCDEFull) == 0x9C)
                 ? 0
                 : 1;
    }
};

int sc_main(int, char**) {
    Top t("t");
    sc_core::sc_start();
    std::printf("fx1-ddr3 package consumer: %s\n", t.rc == 0 ? "PASS" : "FAIL");
    return t.rc;
}
