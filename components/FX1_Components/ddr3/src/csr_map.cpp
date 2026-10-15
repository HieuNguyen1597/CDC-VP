// SPDX-License-Identifier: Apache-2.0
// HAS V1.0 Table 8-5 address summary with the access class, reset and "write zero"
// reserved masks of Tables 8-6..8-18. Fields documented as "writes are ignored" or "read
// zero" are not in rsvd_zero: such bits are dropped, not rejected (GAP-CSR-03).
#include "fx1_ddr3/csr_map.h"

#include "fx1_ddr3/fx1_ddr3_regs.h"

#include <algorithm>
#include <deque>
#include <string>

namespace fx1::ddr3 {

namespace {
using A = CsrAccess;
using S = CsrSource;

std::vector<CsrDesc> build() {
    std::vector<CsrDesc> t;
    auto add = [&t](std::uint32_t off, const char* name, A a, std::uint32_t reset, std::uint32_t rz, S src) {
        t.push_back({static_cast<std::uint16_t>(off), name, a, reset, rz, src});
    };
    // Names of repeated banks are generated once and kept alive for the table lifetime;
    // a deque keeps element addresses stable on push_back (c_str() stays valid).
    static std::deque<std::string> names;
    auto nm = [](const std::string& n) -> const char* {
        names.push_back(n);
        return names.back().c_str();
    };

    // Region 0
    add(FX1_DDR3_IP_ID, "IP_ID", A::RO, 0x44445233u, 0, S::Constant);
    add(FX1_DDR3_ABI_VERSION, "ABI_VERSION", A::RO, 0x00010000u, 0, S::Constant);
    add(FX1_DDR3_CAP_FEATURE, "CAP_FEATURE", A::RO, 0, 0, S::Constant);
    add(FX1_DDR3_CAP_DEPTH0, "CAP_DEPTH0", A::RO, 0x04101008u, 0, S::Constant);
    add(FX1_DDR3_CAP_WIDTH0, "CAP_WIDTH0", A::RO, 0, 0, S::Constant);
    add(FX1_DDR3_GLOBAL_CTRL, "GLOBAL_CTRL", A::RW, 0, 0xFFFFFFFCu, S::Storage);
    add(FX1_DDR3_GLOBAL_STATUS, "GLOBAL_STATUS", A::RO, 0, 0, S::Zero);
    add(FX1_DDR3_QUIESCE_CTRL, "QUIESCE_CTRL", A::WO_CMD, 0, 0xFFFFFFFCu, S::Zero);
    add(FX1_DDR3_QUIESCE_STATUS, "QUIESCE_STATUS", A::RO, 0, 0, S::Live);
    add(FX1_DDR3_RESET_CAUSE, "RESET_CAUSE", A::W1C, 0, 0, S::Storage);
    add(FX1_DDR3_SNAPSHOT_CMD, "SNAPSHOT_CMD", A::WO_CMD, 0, 0xFFFFFFFEu, S::Zero);
    add(FX1_DDR3_SNAPSHOT_STATUS, "SNAPSHOT_STATUS", A::RO, 0, 0, S::Live);
    add(FX1_DDR3_COMMAND_ERROR, "COMMAND_ERROR", A::W1C, 0, 0, S::Storage);
    add(FX1_DDR3_SCRATCH, "SCRATCH", A::RW, 0, 0, S::Storage);
    add(FX1_DDR3_FE_PORT_CMD, "FE_PORT_CMD", A::WO_CMD, 0, 0xFFFFFCF8u, S::Zero);
    add(FX1_DDR3_FE_PORT_CMD_STATUS, "FE_PORT_CMD_STATUS", A::RO, 0, 0, S::Live);
    add(FX1_DDR3_FE_PORT_STATE, "FE_PORT_STATE", A::RO, 0, 0, S::Live);
    for (unsigned i = 0; i < 32; ++i)
        add(FX1_DDR3_FE_REGION_SHADOW(i), nm("FE_REGION_SHADOW" + std::to_string(i)), A::SHADOW, 0, 0, S::Storage);
    add(FX1_DDR3_FE_REGION_COMMIT, "FE_REGION_COMMIT", A::WO_CMD, 0, 0, S::Zero);
    add(FX1_DDR3_FE_REGION_STATUS, "FE_REGION_STATUS", A::RO, 0, 0, S::Live);
    for (unsigned i = 0; i < 32; ++i)
        add(FX1_DDR3_FE_REGION_ACTIVE(i), nm("FE_REGION_ACTIVE" + std::to_string(i)), A::RO, 0, 0, S::Live);

    // Region 1
    add(FX1_DDR3_TOPO_SHADOW, "TOPO_SHADOW", A::SHADOW, 0, 0, S::Storage);
    add(FX1_DDR3_TOPO_ACTIVE, "TOPO_ACTIVE", A::RO, 0, 0, S::Live);
    add(FX1_DDR3_USABLE_GRANULES_LO, "USABLE_GRANULES_LO", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_USABLE_GRANULES_HI, "USABLE_GRANULES_HI", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_PHYSICAL_GRANULES_LO, "PHYSICAL_GRANULES_LO", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_PHYSICAL_GRANULES_HI, "PHYSICAL_GRANULES_HI", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_ECC_BASE_LO, "ECC_BASE_LO", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_ECC_BASE_HI, "ECC_BASE_HI", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_TOPO_COMMIT, "TOPO_COMMIT", A::WO_CMD, 0, 0, S::Zero);
    add(FX1_DDR3_TOPO_COMMIT_STATUS, "TOPO_COMMIT_STATUS", A::RO, 0, 0, S::Live);
    static const char* const tnames[FX1_DDR3_TIMING_WORDS] = {
        "T_RCD", "T_RP", "T_RAS", "T_RC", "T_RRD", "T_FAW", "T_CCD", "T_RTP", "T_WR", "T_WTR", "T_RTW",
        "T_RTRS", "T_RFC", "T_REFI", "CL", "CWL", "ADDITIVE_LATENCY", "PHY_WRITE_LATENCY", "PHY_READ_LATENCY"};
    for (unsigned i = 0; i < FX1_DDR3_TIMING_WORDS; ++i)
        add(FX1_DDR3_TIMING_SHADOW(i), tnames[i], A::SHADOW, 0, 0, S::Storage);
    add(FX1_DDR3_TIMING_COMMIT, "TIMING_COMMIT", A::WO_CMD, 0, 0, S::Zero);
    add(FX1_DDR3_TIMING_COMMIT_STATUS, "TIMING_COMMIT_STATUS", A::RO, 0, 0, S::Live);
    for (unsigned i = 0; i < FX1_DDR3_TIMING_WORDS; ++i)
        add(FX1_DDR3_TIMING_ACTIVE(i), nm(std::string("A_") + tnames[i]), A::RO, 0, 0, S::Live);

    // Region 2
    add(FX1_DDR3_WRITE_DRAIN_CFG, "WRITE_DRAIN_CFG", A::RW, 0x00100408u, 0, S::Storage);
    add(FX1_DDR3_STARVE_LIMIT, "STARVE_LIMIT", A::RW, 0x400u, 0, S::Storage);
    add(FX1_DDR3_SCHED_STATUS, "SCHED_STATUS", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_STAGED_WRITE_OCC, "STAGED_WRITE_OCC", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_WAIT_OLDEST_BE, "WAIT_OLDEST_BE", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_WAIT_OLDEST_SRT, "WAIT_OLDEST_SRT", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_WAIT_OLDEST_HRT, "WAIT_OLDEST_HRT", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_SERVICE_COUNT_BE, "SERVICE_COUNT_BE", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_SERVICE_COUNT_SRT, "SERVICE_COUNT_SRT", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_SERVICE_COUNT_HRT, "SERVICE_COUNT_HRT", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_READ_TIMEOUT, "READ_TIMEOUT", A::RW, 0x400u, 0, S::Storage);
    add(FX1_DDR3_DISPATCH_STATUS, "DISPATCH_STATUS", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_READ_META_STATUS, "READ_META_STATUS", A::RO, 0, 0, S::Snapshot);

    // Region 3
    add(FX1_DDR3_REFRESH_CFG, "REFRESH_CFG", A::RW, 0x8u, 0, S::Storage);
    add(FX1_DDR3_REFRESH_RANK(0), "REFRESH_RANK0", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_REFRESH_RANK(1), "REFRESH_RANK1", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_REFRESH_STATUS, "REFRESH_STATUS", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_INIT_CMD, "INIT_CMD", A::WO_CMD, 0, 0, S::Zero);
    add(FX1_DDR3_INIT_STATUS, "INIT_STATUS", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_INIT_ERROR, "INIT_ERROR", A::W1C, 0, 0, S::Storage);
    for (unsigned i = 0; i < 4; ++i) add(FX1_DDR3_MR(i), nm("MR" + std::to_string(i)), A::INIT_RW, 0, 0, S::Storage);
    add(FX1_DDR3_INIT_RESET_ASSERT_WAIT, "INIT_RESET_ASSERT_WAIT", A::INIT_RW, 1, 0, S::Storage);
    add(FX1_DDR3_INIT_RESET_RELEASE_WAIT, "INIT_RESET_RELEASE_WAIT", A::INIT_RW, 1, 0, S::Storage);
    add(FX1_DDR3_INIT_CKE_LOW_WAIT, "INIT_CKE_LOW_WAIT", A::INIT_RW, 1, 0, S::Storage);
    add(FX1_DDR3_INIT_CKE_HIGH_WAIT, "INIT_CKE_HIGH_WAIT", A::INIT_RW, 1, 0, S::Storage);
    add(FX1_DDR3_INIT_DLL_WAIT, "INIT_DLL_WAIT", A::INIT_RW, 1, 0, S::Storage);
    add(FX1_DDR3_INIT_ZQ_WAIT, "INIT_ZQ_WAIT", A::INIT_RW, 1, 0, S::Storage);
    add(FX1_DDR3_INIT_CMD_TIMEOUT, "INIT_CMD_TIMEOUT", A::INIT_RW, 0x400u, 0, S::Storage);
    add(FX1_DDR3_TRAIN_TRAS_WAIT, "TRAIN_TRAS_WAIT", A::INIT_RW, 0x14u, 0, S::Storage);
    add(FX1_DDR3_POWER_CMD, "POWER_CMD", A::WO_CMD, 0, 0, S::Zero);
    add(FX1_DDR3_POWER_STATUS, "POWER_STATUS", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_POWER_TIMEOUT, "POWER_TIMEOUT", A::RW, 0x400u, 0, S::Storage);
    add(FX1_DDR3_T_CKSRE, "T_CKSRE", A::INIT_RW, 1, 0, S::Storage);
    add(FX1_DDR3_T_CKSRX, "T_CKSRX", A::INIT_RW, 1, 0, S::Storage);
    add(FX1_DDR3_POWER_ERROR, "POWER_ERROR", A::W1C, 0, 0, S::Storage);

    // Region 4
    add(FX1_DDR3_ECC_CAP_STATUS, "ECC_CAP_STATUS", A::RO, 0, 0, S::Live);
    add(FX1_DDR3_CE_THRESHOLD, "CE_THRESHOLD", A::RW, 1, 0, S::Storage);
    add(FX1_DDR3_CE_COUNT, "CE_COUNT", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_UE_COUNT, "UE_COUNT", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_ECC_LAST_ADDR_LO, "ECC_LAST_ADDR_LO", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_ECC_LAST_ADDR_HI, "ECC_LAST_ADDR_HI", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_ECC_LAST_INFO, "ECC_LAST_INFO", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_QUARANTINE_STATUS, "QUARANTINE_STATUS", A::RO, 0, 0, S::Live);
    add(FX1_DDR3_QUARANTINE_FIRST_REQ_LO, "QUARANTINE_FIRST_REQ_LO", A::RO, 0, 0, S::Live);
    add(FX1_DDR3_QUARANTINE_FIRST_REQ_HI, "QUARANTINE_FIRST_REQ_HI", A::RO, 0, 0, S::Live);
    add(FX1_DDR3_FATAL_STATUS, "FATAL_STATUS", A::RO, 0, 0, S::Live);
    add(FX1_DDR3_BACKEND_OCCUPANCY0, "BACKEND_OCCUPANCY0", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_BACKEND_OCCUPANCY1, "BACKEND_OCCUPANCY1", A::RO, 0, 0, S::Snapshot);

    // Region 5
    add(FX1_DDR3_PHY_CAP, "PHY_CAP", A::RO, 0, 0, S::Constant);
    add(FX1_DDR3_PHY_STATUS, "PHY_STATUS", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_DFI_WR_ENABLE_DELAY, "DFI_WR_ENABLE_DELAY", A::SHADOW, 0, 0, S::Storage);
    add(FX1_DDR3_DFI_RD_ENABLE_DELAY, "DFI_RD_ENABLE_DELAY", A::SHADOW, 0, 0, S::Storage);
    add(FX1_DDR3_PHY_MAX_READ_RETURN, "PHY_MAX_READ_RETURN", A::SHADOW, 0x100u, 0, S::Storage);
    add(FX1_DDR3_DFI_ASSEMBLY_MARGIN, "DFI_ASSEMBLY_MARGIN", A::SHADOW, 1, 0, S::Storage);
    add(FX1_DDR3_DFI_TIMING_COMMIT, "DFI_TIMING_COMMIT", A::WO_CMD, 0, 0, S::Zero);
    add(FX1_DDR3_DFI_TIMING_STATUS, "DFI_TIMING_STATUS", A::RO, 0, 0, S::Live);
    add(FX1_DDR3_PHY_RDLVL_DELAY, "PHY_RDLVL_DELAY", A::INIT_RW, 0, 0xFF000000u, S::Storage);
    add(FX1_DDR3_PHY_GATE_DELAY, "PHY_GATE_DELAY", A::INIT_RW, 0, 0xFF000000u, S::Storage);
    add(FX1_DDR3_PHY_WRLVL_DELAY, "PHY_WRLVL_DELAY", A::INIT_RW, 0, 0xFFFFFFF8u, S::Storage);
    add(FX1_DDR3_PHY_SKEW_CTRL0, "PHY_SKEW_CTRL0", A::INIT_RW, 0, 0, S::Storage);
    add(FX1_DDR3_PHY_SKEW_CTRL1, "PHY_SKEW_CTRL1", A::INIT_RW, 0, 0, S::Storage);
    add(FX1_DDR3_TRAIN_CMD, "TRAIN_CMD", A::WO_CMD, 0, 0xF0000000u | FX1_DDR3_TRAIN_CMD_RSVD_MASK, S::Zero);
    add(FX1_DDR3_TRAIN_STATUS, "TRAIN_STATUS", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_TRAIN_TIMEOUT, "TRAIN_TIMEOUT", A::RW, 0x400u, 0, S::Storage);
    add(FX1_DDR3_TRAIN_OBSERVATION0, "TRAIN_OBSERVATION0", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_PHY_COMP_NCTL, "PHY_COMP_NCTL", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_PHY_COMP_PCTL, "PHY_COMP_PCTL", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_PHY_COMP_RXCTL, "PHY_COMP_RXCTL", A::RO, 0, 0, S::Snapshot);
    add(FX1_DDR3_PHY_BRIDGE_ERROR, "PHY_BRIDGE_ERROR", A::W1C, 0, 0, S::Storage);

    // Region 6
    add(FX1_DDR3_IRQ_RAW, "IRQ_RAW", A::RO, 0, 0, S::Live);
    add(FX1_DDR3_IRQ_MASK, "IRQ_MASK", A::RW, 0, 0, S::Storage);
    add(FX1_DDR3_IRQ_STATUS, "IRQ_STATUS", A::RO, 0, 0, S::Live);
    add(FX1_DDR3_IRQ_CLEAR, "IRQ_CLEAR", A::W1C, 0, 0, S::Zero);
    add(FX1_DDR3_IRQ_TEST_SET, "IRQ_TEST_SET", A::WO_CMD, 0, 0, S::Zero);

    std::sort(t.begin(), t.end(), [](const CsrDesc& a, const CsrDesc& b) { return a.offset < b.offset; });
    return t;
}
} // namespace

const std::vector<CsrDesc>& csr_table() {
    static const std::vector<CsrDesc> table = build();
    return table;
}

const CsrDesc* csr_find(std::uint32_t offset) {
    const auto& t = csr_table();
    auto it = std::lower_bound(t.begin(), t.end(), offset,
                               [](const CsrDesc& d, std::uint32_t o) { return d.offset < o; });
    return it != t.end() && it->offset == offset ? &*it : nullptr;
}

const char* to_string(CsrAccess a) noexcept {
    switch (a) {
    case CsrAccess::RO: return "RO";
    case CsrAccess::RW: return "RW";
    case CsrAccess::SHADOW: return "SHADOW";
    case CsrAccess::INIT_RW: return "INIT_RW";
    case CsrAccess::W1C: return "W1C";
    case CsrAccess::WO_CMD: return "WO-CMD";
    }
    return "?";
}

} // namespace fx1::ddr3
