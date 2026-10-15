// SPDX-License-Identifier: Apache-2.0
#include "fx1_ddr3/frontend.h"

#include <algorithm>

namespace fx1::ddr3 {

namespace {
constexpr unsigned kGranule = 32;  // memory service granule (HAS §2.2)
} // namespace

DecodeResult decode_burst(const AxiRequest& r, const RegionMap& map, unsigned bus_bytes) noexcept {
    DecodeResult d;
    const std::uint64_t a = r.addr;
    // FE-6.1.3-01/02: lowest-index enabled hit on the start address.
    for (unsigned i = 0; i < map.size(); ++i) {
        const RegionEntry& e = map[i];
        if (e.size != 0 && a >= e.base && a - e.base < e.size) {
            d.region = static_cast<int>(i);
            d.region_base = e.base;
            d.norm_addr = a - e.base;
            break;
        }
    }
    if (d.region < 0) return d;  // DECERR wins over every other check (FE-6.1.3-11)
    const RegionEntry& reg = map[static_cast<unsigned>(d.region)];
    auto fail = [&d](const char* check) {
        d.resp = AxiResp::SlvErr;
        d.failed = check;
        return d;
    };
    const std::uint64_t beat = std::uint64_t{1} << r.size;
    if (beat > bus_bytes) return fail("size_ok");
    if (r.burst == AxiBurst::Reserved) return fail("burst_ok");
    if (r.burst == AxiBurst::Wrap && r.len != 1 && r.len != 3 && r.len != 7 && r.len != 15) return fail("wrap_ok");
    if (r.burst == AxiBurst::Wrap && a % beat != 0) return fail("wrap_algn");
    if (r.burst == AxiBurst::Fixed && r.len != 0) return fail("fixed_ok");
    const std::uint64_t span = (std::uint64_t{r.len} + 1u) * beat;
    const std::uint64_t aligned = a & ~(beat - 1u);
    if (r.burst == AxiBurst::Incr && (aligned & 0xFFFu) + span > 4096u) return fail("cross_4k_ok");
    // FE-6.1.3-10: every byte of the burst inside the region, no overflow past 2^32.
    std::uint64_t lo = a, hi = 0;
    switch (r.burst) {
    case AxiBurst::Fixed: hi = aligned + beat - 1u; break;
    case AxiBurst::Incr: hi = aligned + span - 1u; break;
    case AxiBurst::Wrap:
        lo = a / span * span;
        hi = lo + span - 1u;
        break;
    case AxiBurst::Reserved: break;
    }
    if (hi > 0xFFFFFFFFull || lo < reg.base || hi >= std::uint64_t{reg.base} + reg.size) return fail("span_contained");
    d.resp = AxiResp::Okay;
    return d;
}

std::uint64_t beat_address(const AxiRequest& r, unsigned n) noexcept {
    const std::uint64_t a = r.addr;
    const std::uint64_t beat = std::uint64_t{1} << r.size;
    switch (r.burst) {
    case AxiBurst::Fixed: return a;
    case AxiBurst::Wrap: {
        const std::uint64_t span = (std::uint64_t{r.len} + 1u) * beat;
        const std::uint64_t base = a / span * span;
        return base + (a - base + n * beat) % span;
    }
    default: return n == 0 ? a : (a & ~(beat - 1u)) + n * beat;
    }
}

std::vector<Atom> plan_atoms(const AxiRequest& r, std::uint32_t region_base, unsigned bus_bytes) {
    std::vector<Atom> atoms;
    const std::uint64_t beat = std::uint64_t{1} << r.size;
    // Capacity only (host cost): a burst spans at most its byte count / 32 + 2 granules.
    atoms.reserve(static_cast<std::size_t>((beat * r.beats()) / kGranule + 2u));
    for (unsigned n = 0; n < r.beats(); ++n) {
        const std::uint64_t addr = beat_address(r, n);
        const std::uint64_t end = (addr & ~(beat - 1u)) + beat;  // narrow/unaligned: bytes addr..end-1
        for (std::uint64_t b = addr; b < end; ++b) {
            const std::uint64_t norm = b - region_base;
            const std::uint64_t granule = norm / kGranule;
            if (atoms.empty() || atoms.back().granule != granule) {
                atoms.emplace_back();
                atoms.back().granule = granule;
                atoms.back().bytes.reserve(static_cast<std::size_t>(std::min<std::uint64_t>(kGranule, beat * r.beats())));
            }
            Atom& at = atoms.back();
            const auto off = static_cast<std::uint8_t>(norm % kGranule);
            at.coverage |= 1u << off;
            at.bytes.push_back({static_cast<std::uint32_t>(n * bus_bytes + b % bus_bytes), off});
        }
    }
    return atoms;
}

} // namespace fx1::ddr3
