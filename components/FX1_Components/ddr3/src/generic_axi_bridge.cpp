// SPDX-License-Identifier: Apache-2.0
#include "fx1_ddr3/generic_axi_bridge.h"

#include <cstring>
#include <stdexcept>
#include <vector>

namespace fx1::ddr3 {

GenericAxiBridge::GenericAxiBridge(sc_core::sc_module_name name, unsigned axi_beat_bytes, std::uint64_t address_offset,
                                   std::uint8_t qos)
    : sc_module(name), beat_bytes_(axi_beat_bytes), offset_(address_offset), qos_(qos) {
    if (beat_bytes_ != 8 && beat_bytes_ != 16) throw std::invalid_argument("GenericAxiBridge: AXI data width is 64 or 128 bits");
    if (qos_ > 15) throw std::invalid_argument("GenericAxiBridge: AxQOS is 4 bits");
    target.register_b_transport(this, &GenericAxiBridge::b_transport);
    target.register_transport_dbg(this, &GenericAxiBridge::transport_dbg);
}

void GenericAxiBridge::b_transport(tlm::tlm_generic_payload& tx, sc_core::sc_time& delay) {
    if (tx.is_read() == tx.is_write()) {  // TLM_IGNORE_COMMAND
        tx.set_response_status(tlm::TLM_OK_RESPONSE);
        return;
    }
    const std::uint64_t length = tx.get_data_length();
    if (!tx.get_data_ptr()) {
        tx.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        return;
    }
    if (tx.get_streaming_width() != 0 && tx.get_streaming_width() < length) {
        tx.set_response_status(tlm::TLM_BURST_ERROR_RESPONSE);
        return;
    }
    const bool write = tx.is_write();
    const std::uint64_t addr = tx.get_address() + offset_;
    const auto plan = plan_generic_access(addr, length, beat_bytes_, write);
    if (plan.empty()) {  // zero length or beyond the 32-bit AXI address space
        tx.set_response_status(length == 0 ? tlm::TLM_OK_RESPONSE : tlm::TLM_ADDRESS_ERROR_RESPONSE);
        return;
    }
    std::uint8_t* data = tx.get_data_ptr();
    const std::uint8_t* be = tx.get_byte_enable_ptr();
    const unsigned be_len = tx.get_byte_enable_length();
    auto enabled = [&](std::uint64_t i) { return !be || be_len == 0 || be[i % be_len] == TLM_BYTE_ENABLED; };

    AxiResp worst = AxiResp::Okay;
    std::vector<std::uint8_t> lanes, strobes;
    for (const PlannedBurst& pb : plan) {
        AxiExtension ext;
        ext.req = pb.req;
        ext.req.qos = qos_;
        const std::uint64_t image = std::uint64_t{ext.req.beats()} * beat_bytes_;
        lanes.assign(image, 0);
        strobes.assign(image, TLM_BYTE_DISABLED);
        // Byte at absolute address a: beat a/W - start/W, lane a mod W (INCR beats after
        // the first are aligned; a narrow single beat keeps its lane).
        const std::uint64_t start = pb.req.addr;
        for (std::uint64_t k = 0; k < pb.byte_count; ++k) {
            const std::uint64_t i = pb.byte_offset + k;
            const std::uint64_t a = start + k;
            const std::uint64_t pos = (a / beat_bytes_ - start / beat_bytes_) * beat_bytes_ + a % beat_bytes_;
            if (write) {
                lanes[pos] = data[i];
                if (enabled(i)) strobes[pos] = TLM_BYTE_ENABLED;
            }
        }
        tlm::tlm_generic_payload sub;
        sub.set_command(write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND);
        sub.set_address(pb.req.addr);
        sub.set_data_ptr(lanes.data());
        sub.set_data_length(static_cast<unsigned>(image));
        sub.set_streaming_width(static_cast<unsigned>(image));
        if (write) {
            sub.set_byte_enable_ptr(strobes.data());
            sub.set_byte_enable_length(static_cast<unsigned>(image));
        }
        sub.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
        sub.set_extension(&ext);
        initiator->b_transport(sub, delay);
        sub.clear_extension(&ext);
        ++transactions_;
        AxiResp r = ext.resp;
        if (sub.get_response_status() != to_tlm_status(r)) r = AxiResp::SlvErr;  // integration error
        if (static_cast<std::uint8_t>(r) > static_cast<std::uint8_t>(worst)) worst = r;
        if (!write) {
            for (std::uint64_t k = 0; k < pb.byte_count; ++k) {
                const std::uint64_t i = pb.byte_offset + k;
                const std::uint64_t a = start + k;
                const std::uint64_t pos = (a / beat_bytes_ - start / beat_bytes_) * beat_bytes_ + a % beat_bytes_;
                if (enabled(i)) data[i] = lanes[pos];
            }
        }
    }
    tx.set_response_status(to_tlm_status(worst));
}

unsigned GenericAxiBridge::transport_dbg(tlm::tlm_generic_payload& tx) {
    const std::uint64_t addr = tx.get_address() + offset_;
    tx.set_address(addr);
    const unsigned n = initiator->transport_dbg(tx);
    tx.set_address(addr - offset_);
    return n;
}

} // namespace fx1::ddr3
