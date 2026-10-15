// SPDX-License-Identifier: Apache-2.0
#pragma once
// Platform adapter: generic TLM payloads of the FX1 bus (CPU 1/2/4/8-byte accesses, DMA
// bursts) -> AXI transactions on one controller memory port (docs/interfaces.md §2.1).
//
//  - plan_generic_access() splits the access: a naturally aligned power-of-two access no
//    wider than the AXI data width is one narrow single-beat INCR; anything else uses
//    full-width INCR bursts with strobes limited to the wanted bytes, split at 4 KiB and
//    at 256 beats. Bursts are issued in address order; all of them are issued even when
//    one fails (no rollback), and the response is the worst one.
//  - Fixed attributes: ID 0 (the controller keeps one port's same-ID traffic in issue
//    order), QoS from the constructor, AxLOCK 0. Platform LR/SC/AMO keep using the
//    platform exclusive monitor; an AMO is one aligned access, hence one transaction.
//  - Address: payload address + `address_offset` (0 when the bus forwards global
//    addresses, DEC-07). Generic byte enables are honoured (TLM repeat rule).
//  - Response: OKAY -> TLM_OK_RESPONSE, SLVERR -> TLM_GENERIC_ERROR_RESPONSE, DECERR ->
//    TLM_ADDRESS_ERROR_RESPONSE. Streaming (streaming width < length) is a burst error.
//  - No DMI; transport_dbg is forwarded with the translated address.
#include "fx1_ddr3/axi_extension.h"

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_initiator_socket.h>
#include <tlm_utils/simple_target_socket.h>

#include <cstdint>

namespace fx1::ddr3 {

class GenericAxiBridge : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<GenericAxiBridge> target{"target"};
    tlm_utils::simple_initiator_socket<GenericAxiBridge> initiator{"initiator"};

    GenericAxiBridge(sc_core::sc_module_name name, unsigned axi_beat_bytes, std::uint64_t address_offset = 0,
                     std::uint8_t qos = 0);

    std::uint64_t transactions() const noexcept { return transactions_; }  // AXI bursts issued

private:
    void b_transport(tlm::tlm_generic_payload& tx, sc_core::sc_time& delay);
    unsigned transport_dbg(tlm::tlm_generic_payload& tx);

    unsigned beat_bytes_;
    std::uint64_t offset_;
    std::uint8_t qos_;
    std::uint64_t transactions_ = 0;
};

} // namespace fx1::ddr3
