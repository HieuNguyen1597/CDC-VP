// SPDX-License-Identifier: Apache-2.0
#pragma once
// Model-side CSR decode table (HAS §8.1 Table 8-5). Offsets come from fx1_ddr3_regs.h; the
// tests check this table against an independent transcription of the register inventory.
#include <cstdint>
#include <vector>

namespace fx1::ddr3 {

enum class CsrAccess : std::uint8_t { RO, RW, SHADOW, INIT_RW, W1C, WO_CMD };

// How a read value is produced.
enum class CsrSource : std::uint8_t {
    Storage,   // plain storage written through the access class (RW/SHADOW/INIT_RW/W1C sticky)
    Constant,  // elaborated constant (capability words)
    Live,      // computed from live state at read time
    Snapshot,  // value of the last acknowledged snapshot capture (0 before the first)
    Zero,      // reads 0 (no storage: WO-CMD, IRQ_CLEAR, unavailable words)
};

struct CsrDesc {
    std::uint16_t offset;
    const char* name;
    CsrAccess access;
    std::uint32_t reset;        // HAS reset / reset read value
    std::uint32_t rsvd_zero;    // "write zero" reserved bits: a nonzero write is SLVERR
    CsrSource source;
};

// All 210 decoded words, ascending offset.
const std::vector<CsrDesc>& csr_table();
// nullptr for an unmapped or reserved address (DECERR).
const CsrDesc* csr_find(std::uint32_t offset);
const char* to_string(CsrAccess a) noexcept;

} // namespace fx1::ddr3
