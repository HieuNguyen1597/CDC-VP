#include "fu3/qspi/qspi_controller.h"
#include <algorithm>
#include <stdexcept>

namespace fu3::qspi {
namespace {
unsigned lines(unsigned mode) { return mode ? 1u << (mode-1) : 0; }
std::uint32_t unpack(const unsigned char* p) {
    std::uint32_t v=0;
    for (unsigned i=0;i<4;++i) v |= std::uint32_t(p[i])<<(8*i);
    return v;
}
void pack(unsigned char* p, std::uint32_t v) {
    for (unsigned i=0;i<4;++i) p[i]=static_cast<unsigned char>(v>>(8*i));
}
}
QspiController::QspiController(sc_core::sc_module_name name, Config config) : sc_module(name), cfg_(config), fifo_(config.tx_fifo_bytes,config.rx_fifo_bytes,config.flag_threshold_unit) {
    if (!cfg_.tx_fifo_bytes || cfg_.tx_fifo_bytes>32 || !cfg_.rx_fifo_bytes ||
        cfg_.rx_fifo_bytes>32 || !cfg_.bus_timeout_cycles)
        throw std::invalid_argument("QSPI FIFO capacities must be 1..32 bytes; timeout must be nonzero");
    ap_qspic.register_b_transport(this, &QspiController::ap_transport);
    ap_qspic.register_transport_dbg(this, &QspiController::debug);
    xip.register_b_transport(this, &QspiController::xip_transport);
    SC_METHOD(tick);
    sensitive << clk.pos() << reset_n.neg();
    SC_METHOD(interface_reset);
    sensitive << preset_n.neg() << hreset_n.neg();
    SC_METHOD(outputs);
    sensitive << changed_;
}
std::uint64_t QspiController::flash_size() const {
    return r_.flash_size();
}
std::uint32_t QspiController::status() const {
    return r_[reg::SR/4] | (busy_ ? reg::BUSY : 0) |
        fifo_.levels();
}
void QspiController::outputs() {
    irq.write(interrupt_.evaluate(r_[reg::CR/4],r_[reg::SR/4],r_[reg::DMACR/4],r_[reg::DMASR/4]));
    dma_rx_req.write(dma_.rx_request());
    dma_tx_req.write(dma_.tx_request());
}
void QspiController::finish_xip(tlm::tlm_response_status response) {
    if (xip_pending_) {
        xip_pending_->set_response_status(response);
        xip_pending_=nullptr;
        xip_done_.notify(sc_core::SC_ZERO_TIME);
    }
}
void QspiController::interface_reset() {
    if (!preset_n.read()) {
        ++p_epoch_;
        if (ap_pending_) {
            ap_pending_->set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
            ap_pending_=nullptr;
            ap_done_.notify(sc_core::SC_ZERO_TIME);
        }
    }
    if (!hreset_n.read()) {
        ++h_epoch_;
        if (op_==Operation::Xip) stop(false);
        finish_xip(tlm::TLM_GENERIC_ERROR_RESPONSE);
    }
    changed_.notify(sc_core::SC_ZERO_TIME);
}
bool QspiController::validate(tlm::tlm_generic_payload& tx, bool mm) {
    tx.set_dmi_allowed(false);
    if (!tx.is_read() && !tx.is_write())
        tx.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
    else if (tx.get_data_length()!=4 || tx.get_streaming_width()<4)
        tx.set_response_status(tlm::TLM_BURST_ERROR_RESPONSE);
    else if (!tx.get_data_ptr())
        tx.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
    else if ((tx.get_address()&3) || tx.get_address()>(mm ? 0xfffffffcULL : 0x3cULL))
        tx.set_response_status(tlm::TLM_ADDRESS_ERROR_RESPONSE);
    else {
        if (tx.get_byte_enable_ptr()) {
            const unsigned n=tx.get_byte_enable_length();
            if (!n) {
                tx.set_response_status(tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE);
                return false;
            }
            for (unsigned i=0;i<4;++i) if (tx.get_byte_enable_ptr()[i%n]!=0xff) {
                tx.set_response_status(tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE);
                return false;
            }
        }
        if (mm && tx.is_write()) {
            tx.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
            return false;
        }
        return true;
    }
    return false;
}
void QspiController::ap_transport(tlm::tlm_generic_payload& tx, sc_core::sc_time& delay) { transport(tx,delay,false); }
void QspiController::xip_transport(tlm::tlm_generic_payload& tx, sc_core::sc_time& delay) { transport(tx,delay,true); }
void QspiController::transport(tlm::tlm_generic_payload& tx, sc_core::sc_time& delay, bool mm) {
    if (!validate(tx,mm)) return;
    auto& lock=mm ? xip_lock_ : ap_lock_;
    auto& reset=mm ? hreset_n : preset_n;
    auto& busclk=mm ? hclk : pclk;
    auto& pending=mm ? xip_pending_ : ap_pending_;
    auto& done=mm ? xip_done_ : ap_done_;
    const auto ce=core_epoch_, be=mm ? h_epoch_ : p_epoch_;
    auto alive=[&] { return reset_n.read() && reset.read() && ce==core_epoch_ && be==(mm?h_epoch_:p_epoch_); };
    if (delay!=sc_core::SC_ZERO_TIME) { sc_core::wait(delay); delay=sc_core::SC_ZERO_TIME; }
    lock.lock();
    if (alive()) {
        sc_core::wait(busclk.posedge_event() | reset.negedge_event() | reset_n.negedge_event());
        if (alive()) {
            pending=&tx;
            tx.set_response_status(tlm::TLM_INCOMPLETE_RESPONSE);
            unsigned elapsed=0;
            while (pending && alive() && elapsed<cfg_.bus_timeout_cycles) {
                sc_core::wait(done | busclk.posedge_event() | reset.negedge_event() | reset_n.negedge_event());
                if (busclk.posedge()) ++elapsed;
            }
            if (pending) {
                pending=nullptr;
                if (mm && op_==Operation::Xip) stop(false);
                tx.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
            }
            if (!alive()) tx.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        } else tx.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
    } else tx.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
    sc_core::wait(sc_core::SC_ZERO_TIME);
    lock.unlock();
}
unsigned QspiController::debug(tlm::tlm_generic_payload& tx) {
    if (!validate(tx,false)) return 0;
    if (!tx.is_read()) { tx.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE); return 0; }
    access(tx,false);
    return 4;
}
void QspiController::access(tlm::tlm_generic_payload& tx, bool effects) {
    const auto off=static_cast<unsigned>(tx.get_address());
    if (tx.is_read()) pack(tx.get_data_ptr(),read(off,effects));
    else if (effects) write(off,unpack(tx.get_data_ptr()));
    tx.set_response_status(tlm::TLM_OK_RESPONSE);
}
std::uint32_t QspiController::read(unsigned off, bool effects) {
    if (off==reg::SR) return status();
    if (off==reg::FCR || off==reg::DMAFCR) return 0;
    if (off!=reg::DR) return r_.read(off);
    if (mode()==2) return last_status_;
    if (mode()!=1) return 0;
    if (fifo_.rx.empty()) {
        if (effects) r_[reg::DMASR/4] |= reg::RXUDRF;
        return 0;
    }
    std::uint32_t value=0;
    const unsigned n=std::min<std::size_t>(4,fifo_.rx.size());
    for (unsigned i=0;i<n;++i) value |= std::uint32_t(fifo_.rx[i])<<(8*i);
    if (effects) {
        for (unsigned i=0;i<n;++i) fifo_.rx.pop_front();
        if (serial_done_ && fifo_.rx.empty()) busy_=false;
    }
    return value;
}
void QspiController::write(unsigned off, std::uint32_t value) {
    if (off==reg::DR) {
        if (!(r_[0]&reg::EN) || mode()!=0 || !data_mode()) return;
        if (!busy_) start(Operation::Write,r_[reg::AR/4]);
        if (op_!=Operation::Write) return;
        const auto n=static_cast<unsigned>(std::min<std::uint64_t>(4,total_-accepted_));
        if (!fifo_.tx.push_word(value,n)) {
            r_[reg::DMASR/4] |= reg::TXOVRF;
            return;
        }
        accepted_+=n;
        return;
    }
    if (!r_.write(off,value,busy_)) return;
    if (off==reg::CR) {
        if (value&reg::ABORT) stop(true);
        else if (!(value&reg::EN)) stop(false);
        return;
    }
    if (off==reg::DMACR) return;
    if (off==reg::CCR) { instruction_sent_=false; serial_done_=false; dma_.arm(); }
    if (off==reg::AR || off==reg::DLR) { dma_.arm(); serial_done_=false; }
    if (!(r_[0]&reg::EN) || mode()==3) return;
    const bool address_phase=((r_[reg::CCR/4]>>10)&3)!=0;
    if ((off==reg::CCR && !address_phase) || (off==reg::AR && address_phase)) {
        if (mode()==0 && !data_mode()) start(Operation::Write,r_[reg::AR/4]);
        if (mode()==1) start(Operation::Read,r_[reg::AR/4]);
        if (mode()==2) start(Operation::Poll,r_[reg::AR/4]);
    }
}
bool QspiController::legal(bool mm) const {
    const auto c=r_[reg::CCR/4];
    if (!QspiTimingModel::legal_divider(r_[0],(c&reg::DDRM)!=0)) return false;
    if (mode()==2 && (r_[reg::DLR/4]>3 || !data_mode())) return false;
    if (mm && (!data_mode() || !((c>>10)&3))) return false;
    // An entirely empty serial frame has no defined effect.
    return ((c>>8)&3) || ((c>>10)&3) || ((c>>14)&3) || data_mode();
}
void QspiController::start(Operation operation, std::uint32_t address) {
    const bool mm=operation==Operation::Xip;
    const auto c=r_[reg::CCR/4];
    address_=((c>>10)&3) ? address : 0;
    total_=mm ? 4 : (data_mode() ? std::uint64_t(r_[reg::DLR/4])+1 : 0);
    if (!mm && mode()!=2 && data_mode() && r_[reg::DLR/4]==0xffffffffu)
        total_=address_<flash_size() ? flash_size()-address_ : 0;
    const unsigned address_bits=8*(((c>>12)&3)+1);
    const bool address_fits=!((c>>10)&3) || (std::uint64_t(address_) < (std::uint64_t{1}<<address_bits));
    if (!legal(mm) || !address_fits || (operation!=Operation::Poll &&
        (address_>=flash_size() || total_>flash_size()-address_)) ||
        (!total_ && data_mode())) {
        if (mm) finish_xip(tlm::TLM_ADDRESS_ERROR_RESPONSE);
        else r_[reg::SR/4] |= reg::TEF;
        return;
    }
    op_=operation;
    dma_.arm(operation==Operation::Write);
    busy_=true;
    serial_done_=false;
    transferred_=accepted_=0;
    chunk_pending_=backend_called_=false;
    timing_.poll_start=timing_.cycles;
    ++frame_id_;
}
std::uint64_t QspiController::phase_cycles(unsigned bytes, unsigned lane_count, bool ddr) const {
    return QspiTimingModel::phase_cycles(bytes,lane_count,ddr,r_[0]);
}
void QspiController::schedule_chunk() {
    chunk_size_=static_cast<unsigned>(std::min<std::uint64_t>(4,total_-transferred_));
    if (op_==Operation::Read) chunk_size_=std::min(chunk_size_,cfg_.rx_fifo_bytes);
    if (op_==Operation::Write && fifo_.tx.size()<chunk_size_) return;
    if (op_==Operation::Read && fifo_.rx.size()+chunk_size_>cfg_.rx_fifo_bytes) return;
    chunk_.fill(0);
    if (op_==Operation::Write) for (unsigned i=0;i<chunk_size_;++i) {
        chunk_[i]=fifo_.tx.front(); fifo_.tx.pop_front();
    }
    const auto c=r_[reg::CCR/4];
    const bool ddr=(c&reg::DDRM)!=0;
    std::uint64_t duration=phase_cycles(chunk_size_,lines(data_mode()),ddr);
    if (!transferred_) {
        if (!(c&reg::SIOO) || !instruction_sent_) duration+=phase_cycles(1,lines((c>>8)&3),false);
        duration+=phase_cycles(((c>>12)&3)+1,lines((c>>10)&3),ddr);
        duration+=phase_cycles(((c>>16)&3)+1,lines((c>>14)&3),ddr);
        duration+=((c>>18)&31)*((r_[0]>>24)+1);
    }
    timing_.ready_cycle=timing_.cycles+std::max<std::uint64_t>(1,duration);
    chunk_pending_=true;
    backend_called_=false;
}
bool QspiController::send_chunk(bool cancel) {
    if (!flash.size()) return false;
    const auto c=r_[reg::CCR/4];
    FlashTransaction meta;
    meta.frame=frame_id_; meta.offset=transferred_; meta.length=total_;
    meta.address=address_; meta.alternate=r_[reg::ABR/4];
    meta.instruction=static_cast<std::uint8_t>(c);
    meta.begin=transferred_==0; meta.end=transferred_+chunk_size_==total_;
    meta.cancel=cancel;
    meta.instruction_lines=meta.begin && (!(c&reg::SIOO)||!instruction_sent_) ? lines((c>>8)&3) : 0;
    meta.address_lines=lines((c>>10)&3); meta.address_bytes=meta.address_lines?((c>>12)&3)+1:0;
    meta.alternate_lines=lines((c>>14)&3); meta.alternate_bytes=meta.alternate_lines?((c>>16)&3)+1:0;
    meta.data_lines=lines(data_mode()); meta.dummy_cycles=(c>>18)&31;
    meta.ddr=(c&reg::DDRM)!=0; meta.clock_mode3=(r_[reg::DCR/4]&1)!=0;
    if (cancel) { meta.begin=false; meta.end=true; }
    tlm::tlm_generic_payload tx;
    tx.set_command(cancel || !chunk_size_ ? tlm::TLM_IGNORE_COMMAND :
        (op_==Operation::Write ? tlm::TLM_WRITE_COMMAND : tlm::TLM_READ_COMMAND));
    tx.set_address(std::uint64_t(address_)+transferred_);
    tx.set_data_ptr(chunk_.data());
    tx.set_data_length(cancel ? 0 : chunk_size_);
    tx.set_streaming_width(cancel ? 0 : chunk_size_);
    tx.set_extension(&meta);
    sc_core::sc_time delay=sc_core::SC_ZERO_TIME;
    try { flash->b_transport(tx,delay); }
    catch (...) { tx.clear_extension<FlashTransaction>(); return false; }
    tx.clear_extension<FlashTransaction>();
    if (!tx.is_response_ok()) return false;
    if (!cancel) {
        instruction_sent_=true;
        frame_open_=!meta.end;
        timing_.backend_ready=sc_core::sc_time_stamp()+delay;
    }
    return true;
}
void QspiController::stop(bool abort, bool error) {
    if (frame_open_) send_chunk(true);
    frame_open_=false;
    if (op_==Operation::Xip) finish_xip(tlm::TLM_GENERIC_ERROR_RESPONSE);
    op_=Operation::None;
    busy_=serial_done_=chunk_pending_=backend_called_=false;
    fifo_.reset();
    dma_.reset();
    r_[0] &= ~reg::ABORT;
    if (abort) r_[reg::SR/4] |= reg::TCF;
    if (error) r_[reg::SR/4] |= reg::TEF;
    timing_.ready_cycle=timing_.cycles+QspiTimingModel::cs_high(r_[0],r_[reg::DCR/4]);
}
void QspiController::complete_chunk() {
    chunk_pending_=false;
    if (op_==Operation::Read) for (unsigned i=0;i<chunk_size_;++i) fifo_.rx.push_back(chunk_[i]);
    transferred_+=chunk_size_;
    if (transferred_<total_) return;
    const auto high_cycles=QspiTimingModel::cs_high(r_[0],r_[reg::DCR/4]);
    timing_.ready_cycle=timing_.cycles+high_cycles;
    if (op_==Operation::Xip) {
        std::copy_n(chunk_.begin(),4,xip_pending_->get_data_ptr());
        finish_xip(tlm::TLM_OK_RESPONSE);
        busy_=false; op_=Operation::None;
    } else if (op_==Operation::Poll) {
        last_status_=unpack(chunk_.data());
        const auto valid=total_==4 ? 0xffffffffu : (std::uint32_t{1}<<(8*total_))-1;
        const auto mask=r_[reg::PSMKR/4]&valid;
        const auto different=(last_status_^r_[reg::PSMAR/4])&mask;
        const bool match=(r_[0]&reg::PMM) ? ((~different&mask)!=0) : different==0;
        if (match) r_[reg::SR/4] |= reg::SMF;
        if (match && (r_[0]&reg::APMS)) { busy_=false; op_=Operation::None; }
        else if ((r_[0]&reg::PTCEN) && timing_.poll_expired(r_[reg::PTR/4])) {
            r_[reg::SR/4] |= reg::STOF; busy_=false; op_=Operation::None;
        } else {
            timing_.ready_cycle=timing_.cycles+QspiTimingModel::poll_gap(r_[0],r_[reg::DCR/4],r_[reg::PIR/4]);
            transferred_=0;
            ++frame_id_;
        }
    } else {
        r_[reg::SR/4] |= reg::TCF;
        serial_done_=true;
        busy_=!fifo_.rx.empty();
        op_=Operation::None;
    }
}
void QspiController::advance() {
    if (op_==Operation::None) return;
    if (chunk_pending_) {
        if (timing_.cycles<timing_.ready_cycle) return;
        if (!backend_called_) {
            if (!send_chunk()) { stop(false,op_!=Operation::Xip); return; }
            backend_called_=true;
        }
        if (timing_.backend_due()) complete_chunk();
    } else {
        if (op_==Operation::Poll && (r_[0]&reg::PTCEN) &&
            timing_.poll_expired(r_[reg::PTR/4]) && timing_.cycles!=timing_.poll_start) {
            r_[reg::SR/4] |= reg::STOF; busy_=false; op_=Operation::None;
        } else if (timing_.cycles>=timing_.ready_cycle) schedule_chunk();
    }
}
void QspiController::tick() {
    if (!reset_n.read()) {
        ++core_epoch_;
        stop(false);
        r_.reset(); last_status_=0; instruction_sent_=false;
        timing_.ready_cycle=timing_.cycles;
        if (ap_pending_) {
            ap_pending_->set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
            ap_pending_=nullptr; ap_done_.notify(sc_core::SC_ZERO_TIME);
        }
        finish_xip(tlm::TLM_GENERIC_ERROR_RESPONSE);
    } else if (clk.posedge()) {
        timing_.tick();
        // Reassert threshold flags on a subsequent core edge after W1C.
        if (r_[0]&reg::EN) {
            if (mode()==0 && fifo_.tx_threshold((r_[0]>>8)&31)) r_[reg::SR/4] |= reg::TXFTF;
            if (mode()==1 && fifo_.rx_threshold((r_[0]>>3)&31)) r_[reg::SR/4] |= reg::RXFTF;
        }
        if (ap_pending_) {
            if (preset_n.read()) access(*ap_pending_,true);
            else ap_pending_->set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
            ap_pending_=nullptr; ap_done_.notify(sc_core::SC_ZERO_TIME);
        }
        if (xip_pending_ && op_!=Operation::Xip) {
            if (!hreset_n.read() || !(r_[0]&reg::EN) || mode()!=3 || busy_)
                finish_xip(tlm::TLM_GENERIC_ERROR_RESPONSE);
            else start(Operation::Xip,static_cast<std::uint32_t>(xip_pending_->get_address()));
        }
        advance();
        QspiDmaInterface::Input input;
        input.cr=r_[0]; input.dmacr=r_[reg::DMACR/4]; input.dlr=r_[reg::DLR/4];
        input.mode=mode(); input.data_mode=data_mode();
        input.rx_bytes=fifo_.rx.size(); input.tx_free=fifo_.tx.free();
        input.writing=op_==Operation::Write; input.busy=busy_; input.serial_done=serial_done_;
        input.rx_clear=dma_rx_clr.read(); input.tx_clear=dma_tx_clr.read();
        input.total=total_; input.accepted=accepted_;
        dma_.tick(input);
    }
    changed_.notify(sc_core::SC_ZERO_TIME);
}
}
