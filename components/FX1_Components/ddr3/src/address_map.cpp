// SPDX-License-Identifier: Apache-2.0
#include "fx1_ddr3/address_map.h"

namespace fx1::ddr3 {

unsigned column_bits(const Geometry& g) noexcept {
    unsigned c = 0;
    while ((8u << c) < g.page_words) ++c;
    return c;
}

bool geometry_possible(const Geometry& g) noexcept {
    const bool page_ok = g.page_words == 256 || g.page_words == 512 || g.page_words == 1024 || g.page_words == 2048;
    return (g.ranks == 1 || g.ranks == 2) && g.row_bits >= 12 && g.row_bits <= 16 && page_ok &&
           g.physical_granules != 0 && g.usable_granules <= g.physical_granules;
}

DramCoord decode_coord(const Geometry& g, std::uint64_t granule) noexcept {
    const unsigned c = column_bits(g);
    const std::uint64_t col_mask = (std::uint64_t{1} << c) - 1u;
    const bool two = g.ranks == 2;
    DramCoord r;
    if (g.map_profile == 0) {  // linear {row, rank, bank, column}
        r.column = static_cast<std::uint32_t>(granule & col_mask);
        r.bank = static_cast<unsigned>((granule >> c) & 7u);
    } else {  // interleaved {row, rank, column, bank}
        r.column = static_cast<std::uint32_t>((granule >> 3) & col_mask);
        r.bank = static_cast<unsigned>(granule & 7u);
    }
    r.rank = two ? static_cast<unsigned>((granule >> (c + 3)) & 1u) : 0u;
    r.row = static_cast<std::uint32_t>(granule >> (c + 3 + (two ? 1u : 0u)));
    return r;
}

MapResult map_granule(const Geometry& g, bool ecc, std::uint64_t d) noexcept {
    MapResult r;  // poisoned: all zero
    if (!geometry_possible(g)) {
        r.status = MapStatus::GeomImpossible;
        return r;
    }
    if (d >= g.usable_granules) {
        r.status = MapStatus::OutOfCapacity;
        return r;
    }
    const DramCoord data = decode_coord(g, d);
    if (data.row >= (std::uint32_t{1} << g.row_bits)) {
        r.status = MapStatus::RowOverflow;
        return r;
    }
    if (ecc) {
        const std::uint64_t e = g.ecc_base + (d >> 3);
        if (e >= g.physical_granules) {
            r.status = MapStatus::RangeFault;
            return r;
        }
        r.ecc_granule = e;
        r.slot = static_cast<unsigned>(d & 7u);
        r.ecc = decode_coord(g, e);
    }
    r.data = data;
    r.status = MapStatus::Ok;
    return r;
}

} // namespace fx1::ddr3
