#include "fu3/aes/aes_controller.h"
#include <stdexcept>

namespace fu3::aes {
using namespace sc_core;
AesController::AesController(sc_module_name name, Config cfg) : sc_module(name), cfg_(cfg) {
    if (!cfg.init_cycles || !cfg.block_cycles)
        throw std::invalid_argument("AES latencies must be at least one clock cycle");
    socket.register_b_transport(this, &AesController::transport);
    socket.register_transport_dbg(this, &AesController::debug);
    SC_METHOD(tick);
    sensitive << clk.pos();
    dont_initialize();
}
void AesController::reset() {
    ++reset_epoch_;
    key_.fill(0); block_.fill(0); result_.fill(0); pending_result_.fill(0);
    core_.clear();
    config_ = command_ = remaining_ = 0;
    initialized_ = valid_ = false;
    job_ = Job::Idle;
}
void AesController::tick() {
    if (!reset_n.read()) { reset(); return; }
    command_ = 0;
    if (job_ == Job::Idle || --remaining_ != 0) return;
    if (job_ == Job::Init) initialized_ = true;
    else { result_ = pending_result_; valid_ = true; }
    job_ = Job::Idle;
}
std::uint32_t AesController::read(unsigned offset) const {
    switch (offset) {
    case reg::NAME0: return 0x61657320;
    case reg::NAME1: return 0x20202020;
    case reg::VERSION: return 0x302e3630;
    case reg::CTRL: return command_ | (config_ << 2);
    case reg::STATUS: return (job_ == Job::Idle ? reg::READY : 0) | (valid_ ? reg::VALID : 0);
    default:
        if (offset >= reg::RESULT0 && offset <= reg::result(3)) {
            const auto i = (offset - reg::RESULT0) / 4;
            if (cfg_.result_readback == ResultReadback::ObservedRtlMux)
                return i == 0 || i == 3 ? result_[3] : 0;
            return result_[i];
        }
        return 0;
    }
}
bool AesController::write(unsigned offset, std::uint32_t value, std::uint32_t mask) {
    if (!mask) return true;
    const auto merge = [=](std::uint32_t old) { return (old & ~mask) | (value & mask); };
    const bool key = offset >= reg::KEY0 && offset <= reg::key(7);
    const bool block = offset >= reg::BLOCK0 && offset <= reg::block(3);
    if (job_ != Job::Idle && (key || block || offset == reg::CONFIG ||
                            (offset == reg::CTRL && (value & mask & 3)))) return false;
    if (key) {
        key_[(offset - reg::KEY0) / 4] = merge(key_[(offset - reg::KEY0) / 4]);
        initialized_ = false;
    } else if (block) {
        block_[(offset - reg::BLOCK0) / 4] = merge(block_[(offset - reg::BLOCK0) / 4]);
    } else if (offset == reg::CONFIG) {
        const auto next = merge(config_) & 3;
        if ((next ^ config_) & reg::KEY256) initialized_ = false;
        config_ = next;
    } else if (offset == reg::CTRL) {
        const auto command = value & mask & 3;
        if (!command) return true;
        if (!(command & reg::INIT) && !initialized_) return false;
        command_ = command;
        valid_ = false;
        if (command & reg::INIT) {
            Key bytes{};
            for (unsigned i = 0; i < bytes.size(); ++i)
                bytes[i] = static_cast<std::uint8_t>(key_[i / 4] >> (24 - 8 * (i % 4)));
            core_.initialize(bytes, (config_ & reg::KEY256) != 0);
            initialized_ = false;
            job_ = Job::Init;
            remaining_ = cfg_.init_cycles;
        } else {
            Block bytes{};
            for (unsigned i = 0; i < bytes.size(); ++i)
                bytes[i] = static_cast<std::uint8_t>(block_[i / 4] >> (24 - 8 * (i % 4)));
            const auto out = core_.transform(bytes, (config_ & reg::ENCRYPT) != 0);
            pending_result_.fill(0);
            for (unsigned i = 0; i < out.size(); ++i)
                pending_result_[i / 4] |= std::uint32_t(out[i]) << (24 - 8 * (i % 4));
            job_ = Job::Block;
            remaining_ = cfg_.block_cycles;
        }
    }
    return true;
}
bool AesController::validate(tlm::tlm_generic_payload& tx) const {
    tx.set_dmi_allowed(false);
    auto error = [&](tlm::tlm_response_status s) { tx.set_response_status(s); return false; };
    if (!tx.is_read() && !tx.is_write()) return error(tlm::TLM_COMMAND_ERROR_RESPONSE);
    if (!tx.get_data_ptr()) return error(tlm::TLM_GENERIC_ERROR_RESPONSE);
    if (tx.get_data_length() != 4 || tx.get_streaming_width() < 4)
        return error(tlm::TLM_BURST_ERROR_RESPONSE);
    if ((tx.get_address() & 3) || tx.get_address() >= reg::APERTURE)
        return error(tlm::TLM_ADDRESS_ERROR_RESPONSE);
    if (const auto* be = tx.get_byte_enable_ptr()) {
        const auto n = tx.get_byte_enable_length();
        if (!n) return error(tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE);
        for (unsigned i = 0; i < 4; ++i)
            if (be[i % n] != 0 && be[i % n] != 0xff)
                return error(tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE);
    }
    return true;
}
void AesController::read_payload(tlm::tlm_generic_payload& tx) const {
    const auto value = read(static_cast<unsigned>(tx.get_address()));
    const auto* be = tx.get_byte_enable_ptr();
    for (unsigned i = 0; i < 4; ++i)
        if (!be || be[i % tx.get_byte_enable_length()])
            tx.get_data_ptr()[i] = static_cast<unsigned char>(value >> (8 * i));
}
void AesController::transport(tlm::tlm_generic_payload& tx, sc_time& delay) {
    if (!validate(tx)) return;
    const auto epoch = reset_epoch_;
    // A request issued in reset stays invalid even if its incoming delay
    // outlasts an unsampled reset pulse (which does not advance reset_epoch_).
    const bool entered_in_reset = !reset_n.read();
    if (delay != SC_ZERO_TIME) { wait(delay); delay = SC_ZERO_TIME; }
    bus_lock_.lock();
    struct Unlock { sc_mutex& mutex; ~Unlock() { mutex.unlock(); } } unlock{bus_lock_};
    const auto cancelled = [&] { return entered_in_reset || epoch != reset_epoch_ || !reset_n.read(); };
    if (cancelled()) { tx.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE); return; }
    wait(clk.posedge_event());
    wait(SC_ZERO_TIME); // Core tick/reset completes before register access.
    if (cancelled()) { tx.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE); return; }
    if (tx.is_read()) read_payload(tx);
    else {
        std::uint32_t value = 0, mask = 0;
        const auto* be = tx.get_byte_enable_ptr();
        for (unsigned i = 0; i < 4; ++i) {
            value |= std::uint32_t(tx.get_data_ptr()[i]) << (8 * i);
            if (!be || be[i % tx.get_byte_enable_length()]) mask |= 0xffu << (8 * i);
        }
        if (!write(static_cast<unsigned>(tx.get_address()), value, mask)) {
            tx.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
            return;
        }
    }
    tx.set_response_status(tlm::TLM_OK_RESPONSE);
}
unsigned AesController::debug(tlm::tlm_generic_payload& tx) {
    if (!validate(tx)) return 0;
    if (!tx.is_read()) { tx.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE); return 0; }
    read_payload(tx);
    tx.set_response_status(tlm::TLM_OK_RESPONSE);
    return 4;
}
}
