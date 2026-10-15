// SPDX-License-Identifier: Apache-2.0
#pragma once
// Functional controller-to-PHY peer contract (HAS Table 1-3/1-4, §9.1.1 BE07/BE08,
// §9.1.2 BE09/BE18). No DFI signal or electrical behaviour is modelled: the provider
// exposes the readiness levels, accepts one acknowledged load transaction at a time and
// returns an observation tagged with rank/epoch/tag.
#include <systemc>

#include <cstdint>

namespace fx1::ddr3 {

// BE07: four active-high levels, synchronized into the controller domain.
struct PhyLevels {
    bool reset_done = false;
    bool ready = false;
    bool failure = false;
    bool timing_wait_ok = false;
};

// Training load classes (HAS Table 9-14: read-level, gate-level, write-level).
enum class PhyLoadClass : std::uint8_t { ReadLevel = 0, GateLevel = 1, WriteLevel = 2 };

struct PhyLoadRequest {
    unsigned rank = 0;
    PhyLoadClass cls = PhyLoadClass::ReadLevel;
    std::uint32_t payload = 0;   // held stable until acknowledgement
    std::uint32_t skew0 = 0;     // PHY_SKEW_CTRL0, opaque (BE18)
    std::uint32_t skew1 = 0;     // PHY_SKEW_CTRL1, opaque (BE18)
    std::uint16_t epoch = 0;     // bridge epoch
    std::uint8_t tag = 0;        // 4-bit tag; 0 = boot convention
    std::uint8_t lane_mask = 0xF;  // byte lanes to load (TRAIN_CMD lanes, SOQ-03); boot = all
    std::uint16_t options = 0;     // TRAIN_CMD[27:12] options, opaque to the controller
};

// BE09: 8-bit read/write-level responses and three 2-bit mode fields.
struct PhyObservation {
    unsigned rank = 0;
    std::uint16_t epoch = 0;
    std::uint8_t tag = 0;
    bool success = false;
    std::uint8_t rdlvl_resp = 0;
    std::uint8_t wrlvl_resp = 0;
    std::uint8_t rdlvl_mode = 0;
    std::uint8_t gate_mode = 0;
    std::uint8_t wrlvl_mode = 0;
};

// Implemented by the controller; the provider calls it from a SystemC process.
class PhyClient {
public:
    virtual ~PhyClient() = default;
    virtual void phy_observation(const PhyObservation& obs) = 0;
    virtual void phy_levels_changed(const PhyLevels& levels) = 0;
};

class PhyProvider {
public:
    virtual ~PhyProvider() = default;
    virtual void attach(PhyClient* client) = 0;
    virtual PhyLevels levels() const = 0;
    // Native lifecycle controls driven by the controller (HAS Table 1-4).
    virtual void set_phy_reset_n(bool reset_n) = 0;
    virtual void set_clock_disable(bool disable) = 0;
    virtual void set_init_complete(bool complete) = 0;  // BE08 notification
    // One load at a time; the observation arrives later through PhyClient (or never,
    // when the provider is scripted to lose it).
    virtual void load(const PhyLoadRequest& req) = 0;
    // Provider-defined words (HAS Table 8-17: "PHY capability and synchronized status
    // words"). They are opaque to the controller: it does not assign or interpret bits.
    // How they are published at PHY_CAP / PHY_STATUS is pending the spec owner (SOQ-12).
    virtual std::uint32_t provider_cap_word() const = 0;
    virtual std::uint32_t provider_status_word() const = 0;
    virtual std::uint16_t comp_nctl() const = 0;
    virtual std::uint16_t comp_pctl() const = 0;
    virtual std::uint8_t comp_rxctl() const = 0;
};

} // namespace fx1::ddr3
