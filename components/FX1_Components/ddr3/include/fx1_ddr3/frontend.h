// SPDX-License-Identifier: Apache-2.0
#pragma once
// Frontend transaction logic (HAS §6.1.3, §6.1.6, §6.1.9; MAS §6.1.3 Table 6-22). Pure C++17.
//
// Decode: the lowest-index enabled region whose half-open range holds the start address
// selects the region; no hit is DECERR whatever else is wrong. A mapped request is then
// checked for SIZE, BURST, WRAP length/alignment, FIXED length, the INCR 4 KiB rule and
// span containment; any failure is SLVERR. Legality uses the global AXI address
// (SOQ-21); data lives at the region-normalized address A - B_r (SOQ-20).
#include "fx1_ddr3/axi_types.h"

#include <array>
#include <cstdint>
#include <vector>

namespace fx1::ddr3 {

struct RegionEntry {
    std::uint32_t base = 0;
    std::uint32_t size = 0;  // 0 disables the entry
};
using RegionMap = std::array<RegionEntry, 4>;

struct DecodeResult {
    AxiResp resp = AxiResp::DecErr;
    int region = -1;                // selected region (DECERR: -1)
    std::uint32_t region_base = 0;
    std::uint64_t norm_addr = 0;    // A - B_r
    const char* failed = nullptr;   // MAS Table 6-22 check name when SLVERR
};

DecodeResult decode_burst(const AxiRequest& r, const RegionMap& map, unsigned bus_bytes) noexcept;

// Global address of beat n (HAS §6.1.6 / §6.1.9): INCR beats after the first are size
// aligned; WRAP follows next = base + ((cur - base + beat) mod span); FIXED repeats.
std::uint64_t beat_address(const AxiRequest& r, unsigned n) noexcept;

// One backend atom: a visit of one 32-byte granule, in beat order. Consecutive beats in
// the same granule share a visit; a WRAP that re-enters its start granule makes a second
// visit (MAS Table 6-41: n_gran = distinct + split).
struct AtomByte {
    std::uint32_t payload;  // byte index in the TLM lane image (beat * bus_bytes + lane)
    std::uint8_t offset;    // byte offset in the granule
};
struct Atom {
    std::uint64_t granule = 0;   // normalized granule index
    std::uint32_t coverage = 0;  // granule bytes addressed by the burst (address derived)
    std::vector<AtomByte> bytes;
};
// `region_base` converts the beat addresses to normalized addresses. The request must
// have passed decode_burst.
std::vector<Atom> plan_atoms(const AxiRequest& r, std::uint32_t region_base, unsigned bus_bytes);

} // namespace fx1::ddr3
