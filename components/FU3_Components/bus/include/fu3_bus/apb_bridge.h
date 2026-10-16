#pragma once
#include <systemc>
#include "fu3_bus/fifo.h"
#include <tlm>
#include <tlm_utils/simple_target_socket.h>
#include <tlm_utils/simple_initiator_socket.h>

namespace fu3_bus {
class ApbBridge : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<ApbBridge> target{"target"};
    tlm_utils::simple_initiator_socket<ApbBridge> out{"out"};
    explicit ApbBridge(sc_core::sc_module_name name,unsigned cycle_ns=10);
private:
    void b_transport(tlm::tlm_generic_payload& tx, sc_core::sc_time& delay);
    unsigned transport_dbg(tlm::tlm_generic_payload& tx);
    Fifo queue_;
    unsigned cycle_ns_;
};
} // namespace fu3_bus
