// SPDX-License-Identifier: Apache-2.0
#include "fx1_ddr3/axi_types.h"

#include <algorithm>

namespace fx1::ddr3 {

const char* to_string(AxiResp r) noexcept {
    switch (r) {
    case AxiResp::Okay: return "OKAY";
    case AxiResp::ExOkay: return "EXOKAY";
    case AxiResp::SlvErr: return "SLVERR";
    case AxiResp::DecErr: return "DECERR";
    }
    return "?";
}

namespace {
unsigned log2u(unsigned v) {
    unsigned r = 0;
    while ((1u << r) < v) ++r;
    return r;
}
bool is_pow2(std::uint64_t v) { return v != 0 && (v & (v - 1)) == 0; }
} // namespace

std::vector<PlannedBurst> plan_generic_access(std::uint64_t addr, std::uint64_t length,
                                              unsigned bus_bytes, bool write) {
    std::vector<PlannedBurst> out;
    if (length == 0 || addr > 0xFFFFFFFFull || length > 0x100000000ull - addr) return out;

    if (is_pow2(length) && length <= bus_bytes && addr % length == 0) {
        PlannedBurst p;
        p.req.write = write;
        p.req.addr = static_cast<std::uint32_t>(addr);
        p.req.size = static_cast<std::uint8_t>(log2u(static_cast<unsigned>(length)));
        p.byte_count = static_cast<std::uint32_t>(length);
        out.push_back(p);
        return out;
    }

    const std::uint8_t full_size = static_cast<std::uint8_t>(log2u(bus_bytes));
    std::uint64_t cur = addr;
    const std::uint64_t end = addr + length;
    while (cur < end) {
        const std::uint64_t page_end = (cur | 0xFFFull) + 1u;
        const std::uint64_t chunk_end = std::min(end, page_end);
        const std::uint64_t first_beat = cur & ~std::uint64_t{bus_bytes - 1u};
        std::uint64_t beats = (chunk_end - first_beat + bus_bytes - 1u) / bus_bytes;
        std::uint64_t stop = chunk_end;
        if (beats > 256) {  // only reachable with buses wider than 16 bytes
            beats = 256;
            stop = first_beat + 256u * bus_bytes;
        }
        PlannedBurst p;
        p.req.write = write;
        p.req.addr = static_cast<std::uint32_t>(cur);
        p.req.size = full_size;
        p.req.len = static_cast<std::uint8_t>(beats - 1u);
        p.byte_offset = cur - addr;
        p.byte_count = static_cast<std::uint32_t>(stop - cur);
        out.push_back(p);
        cur = stop;
    }
    return out;
}

} // namespace fx1::ddr3
