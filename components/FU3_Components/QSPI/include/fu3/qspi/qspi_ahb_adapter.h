#pragma once
#include "registers.h"
#include <systemc>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>

namespace fu3::qspi {
// Functional AXI-to-AHB boundary. The FU3 fabric decomposes bursts before here.
// The supplied wrapper specifies single, aligned, full-lane 32-bit transfers.
class QspiAhbAdapter : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<QspiAhbAdapter> socket{"socket"};
    tlm_utils::simple_initiator_socket<QspiAhbAdapter> ahb{"ahb"};
    explicit QspiAhbAdapter(sc_core::sc_module_name name, std::uint64_t aperture=reg::XIP_SIZE)
        : sc_module(name), aperture_(aperture) {
        socket.register_b_transport(this, &QspiAhbAdapter::transport);
    }
private:
    const std::uint64_t aperture_;
    void transport(tlm::tlm_generic_payload& tx, sc_core::sc_time& delay) {
        tx.set_dmi_allowed(false);
        if (tx.get_address()>=aperture_ || tx.get_data_length()>aperture_-tx.get_address()) {
            tx.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
            return;
        }
        // Qspi serializes and validates requests, including reset epochs for
        // callers waiting in its admission queue. Do not add a second queue.
        ahb->b_transport(tx, delay);
    }
};
}
