// SPDX-License-Identifier: Apache-2.0
#pragma once
// SECDED (72,64) codec of the ECC path (HAS §6.2.17 BE-2.5-01; code from MAS §6.2.17,
// HAS defines no H-matrix). Pure C++17.
//
// Codeword positions 1..71: check bits 0..6 at positions 1, 2, 4, ..., 64; data bits
// 0..63 at the remaining positions 3, 5, 6, 7, 9, ... in order. Check bit i is the XOR
// of every other position whose index has bit i set; check bit 7 is the overall parity
// of positions 1..71. Bit i of the 8-bit check field is check bit i.
//
// A 32-byte granule holds four 64-bit lanes; lane i is bytes 8i..8i+7, little endian
// (GAP-BE-04). Its 32-bit record is {E3, E2, E1, E0} with E0 in bits [7:0]; record slot s
// of the companion granule occupies bytes 4s..4s+3 (bits [32s+31:32s]).
#include <array>
#include <cstdint>

namespace fx1::ddr3 {

constexpr unsigned kGranuleBytes = 32;
constexpr unsigned kEccLanes = 4;
constexpr unsigned kRecordsPerCompanion = 8;

// MAS Table 6-153, evaluated in this order: CLEAN, UNCORRECTABLE (syndrome > 71),
// CORRECTED_DATA (syndrome != 0 with mismatch; a check-bit position flips no data),
// CORRECTED_PARITY (syndrome 0 with mismatch), UNCORRECTABLE (syndrome != 0, no mismatch).
enum class EccClass : std::uint8_t { Clean, CorrectedData, CorrectedParity, Uncorrectable };

struct LaneDecode {
    EccClass cls = EccClass::Clean;
    std::uint64_t data = 0;      // corrected data (input data when not correctable)
    std::uint8_t syndrome = 0;   // 7-bit syndrome
    bool mismatch = false;       // overall parity mismatch
};

std::uint8_t ecc_encode(std::uint64_t data) noexcept;
LaneDecode ecc_decode(std::uint64_t data, std::uint8_t check) noexcept;

// Granule helpers.
std::uint64_t granule_lane(const std::uint8_t* granule, unsigned lane) noexcept;
void set_granule_lane(std::uint8_t* granule, unsigned lane, std::uint64_t value) noexcept;
std::uint32_t ecc_record(const std::uint8_t* granule) noexcept;  // {E3, E2, E1, E0}
inline std::uint8_t record_check(std::uint32_t record, unsigned lane) noexcept {
    return static_cast<std::uint8_t>(record >> (8 * lane));
}

// Decode of a whole granule against its record. `data` is corrected in place for every
// correctable lane; uncorrectable lanes keep the stored bits.
struct GranuleDecode {
    std::array<LaneDecode, kEccLanes> lane{};
    std::uint8_t ce_lanes = 0;   // lanes CORRECTED_DATA or CORRECTED_PARITY (bitmap)
    std::uint8_t ue_lanes = 0;   // lanes UNCORRECTABLE (bitmap)
};
GranuleDecode decode_granule(std::uint8_t* data, std::uint32_t record) noexcept;

} // namespace fx1::ddr3
