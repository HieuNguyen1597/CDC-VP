// SPDX-License-Identifier: Apache-2.0
#pragma once
// Memory-port transaction contract (see docs/interfaces.md §2).
//
// A memory-port b_transport carries exactly one AXI4 transaction:
//  - the extension holds the AXI address-channel request and returns the AXI response;
//  - payload address == req.addr; data is a lane image of (len+1) * beat_lanes bytes,
//    one AXI_DATA_WIDTH/8-byte beat after another, each byte at its bus lane
//    (addr % beat_lanes for the first beat of a narrow/unaligned transfer);
//  - for writes, byte_enable (same length) carries WSTRB; null means all lanes of the
//    active bytes of each beat;
//  - the TLM response status mirrors resp: OKAY/EXOKAY -> TLM_OK_RESPONSE,
//    SLVERR -> TLM_GENERIC_ERROR_RESPONSE, DECERR -> TLM_ADDRESS_ERROR_RESPONSE.
// A payload without this extension is a protocol error at the IP boundary
// (TLM_COMMAND_ERROR_RESPONSE); generic bus traffic uses the platform bridge.
#include "fx1_ddr3/axi_types.h"

#include <tlm>
#include <vector>

namespace fx1::ddr3 {

struct AxiExtension : tlm::tlm_extension<AxiExtension> {
    AxiRequest req;
    AxiResp resp = AxiResp::Okay;          // B response, or worst R beat response
    std::vector<AxiResp> beat_resp;        // per R beat when the target fills it

    tlm::tlm_extension_base* clone() const override { return new AxiExtension(*this); }
    void copy_from(const tlm::tlm_extension_base& other) override {
        *this = static_cast<const AxiExtension&>(other);
    }
};

inline tlm::tlm_response_status to_tlm_status(AxiResp r) noexcept {
    switch (r) {
    case AxiResp::Okay:
    case AxiResp::ExOkay: return tlm::TLM_OK_RESPONSE;
    case AxiResp::SlvErr: return tlm::TLM_GENERIC_ERROR_RESPONSE;
    case AxiResp::DecErr: return tlm::TLM_ADDRESS_ERROR_RESPONSE;
    }
    return tlm::TLM_GENERIC_ERROR_RESPONSE;
}

} // namespace fx1::ddr3
