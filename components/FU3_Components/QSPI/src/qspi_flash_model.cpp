#include "fu3/qspi/qspi_flash_model.h"
#include <algorithm>
#include <fstream>
#include <stdexcept>

namespace fu3::qspi {
namespace {
std::size_t checked_size(const FlashConfig& c) {
    if (c.size_bytes<4096 || c.size_bytes>16*1024*1024 || (c.size_bytes&(c.size_bytes-1)))
        throw std::invalid_argument("Flash size must be a power of two, 4 KiB..16 MiB");
    return c.size_bytes;
}
}
QspiFlashModel::QspiFlashModel(sc_core::sc_module_name name,FlashConfig config)
    : sc_module(name),cfg_(config),memory_(checked_size(config),0xff) {
    socket.register_b_transport(this,&QspiFlashModel::transport);
    SC_METHOD(service);
    sensitive << reset_n.neg() << completion_;
}
void QspiFlashModel::load(const std::vector<unsigned char>& bytes,std::size_t offset) {
    if (busy() || active_) throw std::logic_error("Cannot load an active flash");
    if (offset>memory_.size() || bytes.size()>memory_.size()-offset) throw std::out_of_range("Flash image range");
    std::copy(bytes.begin(),bytes.end(),memory_.begin()+offset);
}
void QspiFlashModel::load_image(const std::string& name,std::size_t offset) {
    std::ifstream input(name,std::ios::binary|std::ios::ate);
    if (!input) throw std::runtime_error("Cannot open flash image: "+name);
    const auto length=input.tellg();
    if (length<0 || offset>memory_.size() || static_cast<std::uint64_t>(length)>memory_.size()-offset)
        throw std::out_of_range("Flash image too large");
    std::vector<unsigned char> bytes(static_cast<std::size_t>(length));
    input.seekg(0);
    if (!bytes.empty() && !input.read(reinterpret_cast<char*>(bytes.data()),static_cast<std::streamsize>(bytes.size())))
        throw std::runtime_error("Cannot read complete flash image");
    load(bytes,offset);
}
void QspiFlashModel::save_image(const std::string& name) const {
    if (busy() || active_) throw std::logic_error("Cannot save an active flash");
    std::ofstream output(name,std::ios::binary|std::ios::trunc);
    if (!output || !output.write(reinterpret_cast<const char*>(memory_.data()),static_cast<std::streamsize>(memory_.size())))
        throw std::runtime_error("Cannot save flash image: "+name);
    output.close();
    if (!output) throw std::runtime_error("Cannot flush flash image: "+name);
}
std::vector<unsigned char> QspiFlashModel::read_image(std::size_t offset,std::size_t length) const {
    if (offset>memory_.size() || length>memory_.size()-offset) throw std::out_of_range("Flash image range");
    return {memory_.begin()+offset,memory_.begin()+offset+length};
}
bool QspiFlashModel::is_read(std::uint8_t c) { return c==3 || c==0x0b || c==0x3b || c==0x6b || c==0xbb || c==0xeb; }
bool QspiFlashModel::is_program(std::uint8_t c) { return c==2 || c==0x32; }
bool QspiFlashModel::is_erase(std::uint8_t c) { return c==0x20 || c==0xd8 || c==0xc7 || c==0x60; }
void QspiFlashModel::reset_volatile() {
    completion_.cancel();
    pending_=Pending::None;
    active_=wel_=qe_=reset_armed_=continuous_=false;
    next_=0;
}
void QspiFlashModel::finish_due() {
    if (!busy() || sc_core::sc_time_stamp()<deadline_) return;
    if (pending_==Pending::Program) {
        for (unsigned i=0;i<256;++i) memory_[pending_base_+i]&=program_[i];
    } else if (pending_==Pending::Erase) {
        std::fill_n(memory_.begin()+pending_base_,pending_length_,0xff);
    } else if (pending_==Pending::Status) qe_=pending_qe_;
    pending_=Pending::None;
}
void QspiFlashModel::service() {
    if (!reset_n.read()) reset_volatile();
    else finish_due();
}
void QspiFlashModel::schedule(Pending operation,sc_core::sc_time duration,sc_core::sc_time delay) {
    pending_=operation; wel_=false;
    deadline_=sc_core::sc_time_stamp()+delay+duration;
    completion_.notify(delay+duration);
}
bool QspiFlashModel::validate_frame(const FlashTransaction& m,tlm::tlm_command command) const {
    if (m.ddr || (m.instruction_lines!=1 && m.instruction_lines!=0)) return false;
    const bool array=is_read(opcode_) || is_program(opcode_) || opcode_==0x20 || opcode_==0xd8;
    if (array) {
        unsigned address_lines=opcode_==0xbb?2:(opcode_==0xeb?4:1);
        if (m.address_lines!=address_lines || m.address_bytes!=3 || m.address>=memory_.size()) return false;
    } else if (m.address_lines || m.address_bytes) return false;
    const bool io=opcode_==0xbb || opcode_==0xeb;
    if (io) {
        if (m.alternate_lines!=(opcode_==0xbb?2u:4u) || m.alternate_bytes!=1) return false;
    } else if (m.alternate_lines || m.alternate_bytes) return false;
    const unsigned dummy=opcode_==0x0b || opcode_==0x3b || opcode_==0x6b ? 8 : (opcode_==0xeb?4:0);
    if (m.dummy_cycles!=dummy) return false;
    const bool quad=opcode_==0x6b || opcode_==0xeb || opcode_==0x32;
    if (quad && !qe_) return false;
    if (is_read(opcode_)) {
        const unsigned data_lines=quad?4:(opcode_==0x3b || opcode_==0xbb?2:1);
        return command==tlm::TLM_READ_COMMAND && m.data_lines==data_lines && m.length &&
            m.length<=memory_.size()-m.address;
    }
    if (is_program(opcode_)) return wel_ && command==tlm::TLM_WRITE_COMMAND &&
        m.data_lines==(quad?4u:1u) && m.length>=1 && m.length<=256;
    if (opcode_==5 || opcode_==0x35 || opcode_==0x9f)
        return command==tlm::TLM_READ_COMMAND && m.data_lines==1 && m.length>=1;
    if (opcode_==0x31) return wel_ && command==tlm::TLM_WRITE_COMMAND && m.data_lines==1 && m.length==1;
    if (is_erase(opcode_)) {
        if (!wel_) return false;
        if (opcode_==0xd8 && memory_.size()<65536) return false;
    } else if (opcode_!=6 && opcode_!=4 && opcode_!=0x66 && opcode_!=0x99) return false;
    return command==tlm::TLM_IGNORE_COMMAND && !m.data_lines && !m.length;
}
bool QspiFlashModel::same_frame(const FlashTransaction& m) const {
    return m.frame==frame_.frame && m.length==frame_.length && m.address==frame_.address &&
        m.instruction==frame_.instruction && m.address_lines==frame_.address_lines &&
        m.address_bytes==frame_.address_bytes && m.alternate_lines==frame_.alternate_lines &&
        m.alternate_bytes==frame_.alternate_bytes && m.alternate==frame_.alternate &&
        m.data_lines==frame_.data_lines && m.dummy_cycles==frame_.dummy_cycles &&
        m.ddr==frame_.ddr && m.clock_mode3==frame_.clock_mode3 && !m.instruction_lines;
}
void QspiFlashModel::end_frame(sc_core::sc_time delay) {
    active_=false;
    if (opcode_==6) wel_=true;
    else if (opcode_==4) wel_=false;
    else if (opcode_==0x66) reset_armed_=true;
    else if (opcode_==0x99) reset_volatile();
    else if (is_program(opcode_)) {
        program_=staged_; pending_base_=frame_.address&~std::size_t(255);
        schedule(Pending::Program,cfg_.program_time,delay);
    } else if (is_erase(opcode_)) {
        pending_length_=opcode_==0x20 ? 4096 : (opcode_==0xd8 ? 65536 : memory_.size());
        pending_base_=opcode_==0x20 || opcode_==0xd8 ? frame_.address&~(pending_length_-1) : 0;
        const auto duration=opcode_==0x20?cfg_.sector_erase_time:(opcode_==0xd8?cfg_.block_erase_time:cfg_.chip_erase_time);
        schedule(Pending::Erase,duration,delay);
    } else if (opcode_==0x31) {
        pending_qe_=(staged_[0]&2)!=0;
        schedule(Pending::Status,cfg_.status_write_time,delay);
    }
    if (opcode_==0xeb) continuous_=(frame_.alternate&0xf0)==0xa0;
}
void QspiFlashModel::transport(tlm::tlm_generic_payload& tx,sc_core::sc_time& delay) {
    tx.set_dmi_allowed(false);
    auto fail=[&](tlm::tlm_response_status code) {
        tx.set_response_status(code);
        // An invalid continuation closes its own frame, never another frame.
        auto* m=tx.get_extension<FlashTransaction>();
        if (m && active_ && m->frame==frame_.frame) {
            if (is_program(opcode_) || opcode_==0x31) wel_=false;
            active_=false;
        }
    };
    if (!reset_n.read()) { fail(tlm::TLM_GENERIC_ERROR_RESPONSE); return; }
    finish_due();
    const auto* m=tx.get_extension<FlashTransaction>();
    if (!m) { fail(tlm::TLM_GENERIC_ERROR_RESPONSE); return; }
    if (m->cancel) {
        if (!active_ || m->frame!=frame_.frame || tx.get_data_length()!=0 ||
            m->begin || !m->end || tx.get_command()!=tlm::TLM_IGNORE_COMMAND) {
            fail(tlm::TLM_GENERIC_ERROR_RESPONSE); return;
        }
        if (is_program(opcode_) || opcode_==0x31) wel_=false;
        active_=false; tx.set_response_status(tlm::TLM_OK_RESPONSE); return;
    }
    const auto n=tx.get_data_length();
    if (n>4 || tx.get_streaming_width()<n) { fail(tlm::TLM_BURST_ERROR_RESPONSE); return; }
    if (tx.get_byte_enable_ptr()) { fail(tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE); return; }
    if ((n && !tx.get_data_ptr()) || (!n && m->length) || m->offset>m->length ||
        n>m->length-m->offset || m->end!=(n==m->length-m->offset) ||
        tx.get_address()!=std::uint64_t(m->address)+m->offset) {
        fail(tlm::TLM_GENERIC_ERROR_RESPONSE); return;
    }
    if (m->begin) {
        if (active_ || m->offset) { fail(tlm::TLM_GENERIC_ERROR_RESPONSE); return; }
        if (!m->instruction_lines && !continuous_) { fail(tlm::TLM_COMMAND_ERROR_RESPONSE); return; }
        opcode_=m->instruction_lines ? m->instruction : 0xeb;
        if (busy() && opcode_!=5 && opcode_!=0x35 && opcode_!=0x66 && opcode_!=0x99) {
            fail(tlm::TLM_GENERIC_ERROR_RESPONSE); return;
        }
        if ((opcode_==0x99 && !reset_armed_) || !validate_frame(*m,tx.get_command())) {
            fail(tlm::TLM_COMMAND_ERROR_RESPONSE); return;
        }
        if (opcode_!=0x99) reset_armed_=false;
        if (m->instruction_lines) continuous_=false;
        frame_=*m; next_=0; active_=true; staged_.fill(0xff);
    } else if (!active_ || !same_frame(*m) || m->offset!=next_) {
        fail(tlm::TLM_GENERIC_ERROR_RESPONSE); return;
    }
    // Continuations must preserve the operation direction as well as metadata.
    const bool reading=is_read(opcode_) || opcode_==5 || opcode_==0x35 || opcode_==0x9f;
    const bool writing=is_program(opcode_) || opcode_==0x31;
    if ((reading && !tx.is_read()) || (writing && !tx.is_write())) { fail(tlm::TLM_COMMAND_ERROR_RESPONSE); return; }
    if (opcode_==0x31 && (tx.get_data_ptr()[0]&~2u)) { fail(tlm::TLM_COMMAND_ERROR_RESPONSE); return; }
    for (unsigned i=0;i<n;++i) {
        const auto offset=m->offset+i;
        if (is_read(opcode_)) tx.get_data_ptr()[i]=memory_[m->address+offset];
        else if (opcode_==5) tx.get_data_ptr()[i]=status1();
        else if (opcode_==0x35) tx.get_data_ptr()[i]=status2();
        else if (opcode_==0x9f) tx.get_data_ptr()[i]=cfg_.jedec_id[offset%3];
        else if (is_program(opcode_)) staged_[(m->address+offset)&255]&=tx.get_data_ptr()[i];
        else if (opcode_==0x31) staged_[0]=tx.get_data_ptr()[i];
    }
    next_+=n;
    if (m->end) end_frame(delay);
    tx.set_response_status(tlm::TLM_OK_RESPONSE);
}
}
