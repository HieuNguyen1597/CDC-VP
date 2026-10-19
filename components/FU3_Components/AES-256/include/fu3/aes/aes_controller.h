#pragma once
#include "aes_core.h"
#include "registers.h"
#include <systemc>
#include <tlm_utils/simple_target_socket.h>

namespace fu3::aes {
enum class ResultReadback { FullBlock, ObservedRtlMux };
struct Config {
    unsigned init_cycles = 16;
    unsigned block_cycles = 64;
    ResultReadback result_readback = ResultReadback::FullBlock;
};

class AesController : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<AesController> socket{"socket"};
    sc_core::sc_in<bool> clk{"clk"}, reset_n{"reset_n"};
    SC_HAS_PROCESS(AesController);
    explicit AesController(sc_core::sc_module_name, Config = {});
private:
    enum class Job { Idle, Init, Block };
    Config cfg_;
    AesCore core_;
    std::array<std::uint32_t, 8> key_{};
    std::array<std::uint32_t, 4> block_{}, result_{}, pending_result_{};
    std::uint32_t config_ = 0, command_ = 0;
    unsigned remaining_ = 0;
    std::uint64_t reset_epoch_ = 0;
    bool initialized_ = false, valid_ = false;
    Job job_ = Job::Idle;
    sc_core::sc_mutex bus_lock_;
    void tick();
    void reset();
    void transport(tlm::tlm_generic_payload&, sc_core::sc_time&);
    unsigned debug(tlm::tlm_generic_payload&);
    bool validate(tlm::tlm_generic_payload&) const;
    std::uint32_t read(unsigned) const;
    bool write(unsigned, std::uint32_t value, std::uint32_t mask);
    void read_payload(tlm::tlm_generic_payload&) const;
};
using Aes = AesController;
}
