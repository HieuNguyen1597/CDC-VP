#pragma once
// DDR_SYSCTRL: FX1 VP SoC glue around the DDR3 controller (DDR3 VP plan DEC-04). Register
// map: fx1/fx1_ddr_sysctrl.h. It mirrors the controller's output levels, runs P-channel
// handshakes for firmware (with an automatic wake on PACTIVE), programs the QoS and
// reserved-rate sideband and sequences the cold and coordinated core resets. It is not
// part of the DDR3 IP and holds no fault injection.

#include <cstdint>

#include <systemc>
#include <tlm>
#include <tlm_utils/simple_target_socket.h>

#include <fx1_ddr3/ddr3_controller.h>

namespace cdc::platforms::fx1_soc {

class DdrSysctrl : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<DdrSysctrl> socket{"socket"};

    SC_HAS_PROCESS(DdrSysctrl);
    // `preinit` is reported in STATUS[8] (the platform initialized the DDR itself).
    DdrSysctrl(sc_core::sc_module_name name, fx1::ddr3::Ddr3Controller& ddr, bool preinit);

    std::uint32_t entries() const noexcept { return entries_; }
    std::uint32_t auto_wakes() const noexcept { return auto_wakes_; }
    std::uint32_t wake_denials() const noexcept { return wake_denials_; }
    unsigned cold_resets() const noexcept { return cold_resets_; }
    unsigned core_resets() const noexcept { return core_resets_; }

private:
    void b_transport(tlm::tlm_generic_payload& tx, sc_core::sc_time& delay);
    unsigned transport_dbg(tlm::tlm_generic_payload& tx);
    // false: the offset is not decoded.
    bool read_word(std::uint32_t offset, std::uint32_t& value) const;
    tlm::tlm_response_status write_word(std::uint32_t offset, std::uint32_t value);
    void sequencer();  // SC_THREAD: P-channel handshakes and reset sequences
    void handshake(std::uint8_t pstate);
    void reset_sequence(std::uint32_t op);
    void clear_wake_backoff();

    fx1::ddr3::Ddr3Controller& ddr_;
    const bool preinit_;
    bool auto_wake_ = true;
    bool busy_ = false, accepted_ = false, denied_ = false;
    bool start_pending_ = false;
    std::uint8_t pending_state_ = 0, last_state_ = 0;
    std::uint32_t entries_ = 0, auto_wakes_ = 0;
    // A denied automatic wake is retried after a back-off in simulated time (review S5-R1):
    // a denial can repeat at once (recovery required, mutex owner) while PACTIVE stays high.
    bool wake_denied_ = false;
    sc_core::sc_time wake_backoff_ = sc_core::SC_ZERO_TIME, wake_retry_at_ = sc_core::SC_ZERO_TIME;
    std::uint32_t wake_denials_ = 0;
    std::uint32_t rst_pending_ = 0;  // RST_CTRL bit of the sequence to start
    bool rst_busy_ = false, rst_done_ = false, rst_safe_timeout_ = false;
    unsigned cold_resets_ = 0, core_resets_ = 0;
    std::uint32_t rsv_rate_[4] = {}, rsv_depth_[4] = {};
    sc_core::sc_event start_;
};

} // namespace cdc::platforms::fx1_soc
