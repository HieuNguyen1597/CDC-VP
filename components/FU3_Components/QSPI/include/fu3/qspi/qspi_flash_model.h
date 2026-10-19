#pragma once
#include "flash_transaction.h"
#include <array>
#include <string>
#include <vector>
#include <systemc>
#include <tlm_utils/simple_target_socket.h>

namespace fu3::qspi {
// Generic VP NOR profile, not an emulation of a particular vendor part.
struct FlashConfig {
    std::size_t size_bytes=128*1024;
    std::array<unsigned char,3> jedec_id{{0x7f,0x51,0x11}}; // Synthetic/configurable ID.
    sc_core::sc_time program_time{50,sc_core::SC_US};
    sc_core::sc_time sector_erase_time{200,sc_core::SC_US};
    sc_core::sc_time block_erase_time{500,sc_core::SC_US};
    sc_core::sc_time chip_erase_time{2,sc_core::SC_MS};
    sc_core::sc_time status_write_time{5,sc_core::SC_US};
};

class QspiFlashModel : public sc_core::sc_module {
public:
    tlm_utils::simple_target_socket<QspiFlashModel> socket{"socket"};
    sc_core::sc_in<bool> reset_n{"reset_n"};
    SC_HAS_PROCESS(QspiFlashModel);
    explicit QspiFlashModel(sc_core::sc_module_name,FlashConfig = {});
    std::size_t size() const { return memory_.size(); }
    bool busy() const { return pending_!=Pending::None; }
    std::uint8_t status1() const { return (busy()?1:0)|(wel_?2:0); }
    std::uint8_t status2() const { return qe_?2:0; }
    void load(const std::vector<unsigned char>& bytes,std::size_t offset=0);
    void load_image(const std::string& filename,std::size_t offset=0);
    void save_image(const std::string& filename) const;
    std::vector<unsigned char> read_image(std::size_t offset,std::size_t length) const;
private:
    enum class Pending { None, Program, Erase, Status };
    FlashConfig cfg_;
    std::vector<unsigned char> memory_;
    bool wel_=false,qe_=false,reset_armed_=false,continuous_=false,active_=false;
    FlashTransaction frame_;
    std::uint8_t opcode_=0;
    std::uint64_t next_=0;
    std::array<unsigned char,256> staged_{},program_{};
    Pending pending_=Pending::None;
    std::size_t pending_base_=0,pending_length_=0;
    bool pending_qe_=false;
    sc_core::sc_time deadline_=sc_core::SC_ZERO_TIME;
    sc_core::sc_event completion_;
    void transport(tlm::tlm_generic_payload&,sc_core::sc_time&);
    void service();
    void reset_volatile();
    void finish_due();
    void schedule(Pending,sc_core::sc_time,sc_core::sc_time);
    bool validate_frame(const FlashTransaction&,tlm::tlm_command) const;
    bool same_frame(const FlashTransaction&) const;
    void end_frame(sc_core::sc_time);
    static bool is_read(std::uint8_t);
    static bool is_program(std::uint8_t);
    static bool is_erase(std::uint8_t);
};
}
