// SPDX-License-Identifier: Apache-2.0
#pragma once
// Backend address mapping (HAS §2.5, §6.2.2 Tables 6-30..6-33; MAS §6.2.2 "Address
// decode"). Pure C++17.
//
// One decode function serves the data granule d and its companion ECC granule:
//   C = log2(Wp / 8)          granules per row = 2^C
//   linear:       column = g mod 2^C,          bank = (g >> C) mod 8
//   interleaved:  column = (g >> 3) mod 2^C,   bank = g mod 8
//   rank = 2 ranks ? (g >> (C + 3)) mod 2 : 0
//   row  = g >> (C + 3 + (2 ranks ? 1 : 0))
//   ECC granule e = ECC_BASE + (d >> 3), record slot s = d mod 8
// The model stores data by granule index (the "physical granule"), so the coordinates
// drive bank timing only (BE-6.2.2-19: a sparse store keyed by granule does not alias).
#include "fx1_ddr3/config_checks.h"

#include <cstdint>

namespace fx1::ddr3 {

struct DramCoord {
    unsigned rank = 0;
    unsigned bank = 0;
    std::uint32_t row = 0;
    std::uint32_t column = 0;  // granule column (BL8 burst index within the row)
    bool operator==(const DramCoord& o) const noexcept {
        return rank == o.rank && bank == o.bank && row == o.row && column == o.column;
    }
};

// HAS Table 6-33 status, first match: impossible geometry, then out of capacity / row
// overflow (DECERR, poison), then companion range fault (SLVERR, poison, error event).
enum class MapStatus : std::uint8_t { Ok, GeomImpossible, OutOfCapacity, RowOverflow, RangeFault };

struct MapResult {
    MapStatus status = MapStatus::GeomImpossible;
    DramCoord data{};
    DramCoord ecc{};               // companion coordinates (zero without ECC)
    std::uint64_t ecc_granule = 0; // e = ECC_BASE + (d >> 3) (zero without ECC)
    unsigned slot = 0;             // record slot d mod 8 (zero without ECC)
};

// Startup scrub write order (HAS BE03, MAS Table 6-164): per group of 8 data granules,
// writes 0..7 zero the data granules g..g+7 and write 8 zeroes their companion granule.
// After `writes` accepted writes, data granules [0, data) and companions [0, companions)
// (relative to ECC_BASE) are scrubbed. N_total = U + U/8.
struct ScrubFrontier {
    std::uint64_t data;
    std::uint64_t companions;
};
constexpr ScrubFrontier scrub_frontier(std::uint64_t writes) noexcept {
    return {writes / 9 * 8 + writes % 9, writes / 9};
}

unsigned column_bits(const Geometry& g) noexcept;   // C
bool geometry_possible(const Geometry& g) noexcept;
DramCoord decode_coord(const Geometry& g, std::uint64_t granule) noexcept;
// Every coordinate is forced to 0 when the status is not Ok (poison).
MapResult map_granule(const Geometry& g, bool ecc, std::uint64_t d) noexcept;

} // namespace fx1::ddr3
