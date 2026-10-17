#include "fu3/gpio/gpio.h"

namespace fu3::gpio {
Gpio::Gpio(sc_core::sc_module_name name) : sc_module(name) {
    for (unsigned i = 8; i < 16; ++i)
        words_[i] = reg::PADCFG_RESET;
    socket.register_b_transport(this, &Gpio::b_transport);
    socket.register_transport_dbg(this, &Gpio::transport_dbg);
    SC_METHOD(tick);
    sensitive << clk.pos() << reset_n.neg();
    SC_METHOD(outputs);
    sensitive << changed_ << gpio_in;
}
void Gpio::tick() {
    if (!reset_n.read()) {
        ++reset_generation_;
        words_.fill(0);
        for (unsigned i = 8; i < 16; ++i)
            words_[i] = reg::PADCFG_RESET;
        sync0_ = sync1_ = sample_ = 0;
        irq_ = false;
        if (pending_) {
            pending_->set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
            pending_ = nullptr;
            completed_.notify(sc_core::SC_ZERO_TIME);
        }
    } else if (clk.posedge()) {
        const auto t0 = words_[4], t1 = words_[5];
        const std::uint32_t active =
            words_[3] & ((~t1 & ~t0 & sample_) | (~t1 & t0 & ~sample_) |
                         (t1 & ~t0 & sync1_ & ~sample_) | (t1 & t0 & ~sync1_ & sample_));
        const bool was_pending = irq_;
        // Read data is the pre-edge state, matching combinational APB PRDATA.
        if (pending_)
            transfer(*pending_, true);
        const bool clear = pending_ && pending_->is_read() &&
                           ((pending_->get_address() >> 2) & 15) == 6 && was_pending;
        if (clear) {
            irq_ = false;
            words_[6] = 0;
        } else if (!was_pending && active) {
            irq_ = true;
            words_[6] = active;
        }
        sample_ = sync1_;
        sync1_ = sync0_;
        sync0_ = gpio_in.read().to_uint();
        words_[1] = sample_;
        if (pending_) {
            pending_ = nullptr;
            completed_.notify(sc_core::SC_ZERO_TIME);
        }
    }
    changed_.notify(sc_core::SC_ZERO_TIME);
}
void Gpio::outputs() {
    gpio_out.write(words_[2]);
    gpio_dir.write(words_[0]);
    gpio_in_sync.write(sync1_);
    interrupt.write(irq_);
    for (unsigned i = 0; i < 32; ++i)
        gpio_padcfg[i].write((words_[8 + i / 4] >> (8 * (i % 4))) & 63);
    bool wake = false;
    const auto raw = gpio_in.read().to_uint();
    for (unsigned i = 0; i < 32; ++i)
        if (words_[7] & (std::uint32_t{1} << i))
            wake = (raw >> i) & 1;
    power_event.write(wake);
}
bool Gpio::validate(tlm::tlm_generic_payload& tx) {
    tx.set_dmi_allowed(false);
    if (!tx.is_read() && !tx.is_write())
        tx.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
    else if (tx.get_byte_enable_ptr())
        tx.set_response_status(tlm::TLM_BYTE_ENABLE_ERROR_RESPONSE);
    else if (tx.get_data_length() != 4 || tx.get_streaming_width() < 4)
        tx.set_response_status(tlm::TLM_BURST_ERROR_RESPONSE);
    else if (!tx.get_data_ptr())
        tx.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
    else
        return true;
    return false;
}
void Gpio::transfer(tlm::tlm_generic_payload& tx, bool side_effect) {
    const unsigned index = (tx.get_address() >> 2) & 15;
    auto* data = tx.get_data_ptr();
    if (tx.is_read()) {
        const auto value = words_[index];
        for (unsigned i = 0; i < 4; ++i)
            data[i] = (value >> (8 * i)) & 255;
    } else if (side_effect && index != 1 && index != 6) {
        std::uint32_t value = 0;
        for (unsigned i = 0; i < 4; ++i)
            value |= std::uint32_t(data[i]) << (8 * i);
        words_[index] = value & (index >= 8 ? reg::PADCFG_MASK : 0xffffffffu);
    }
    tx.set_response_status(tlm::TLM_OK_RESPONSE);
}
void Gpio::b_transport(tlm::tlm_generic_payload& tx, sc_core::sc_time& delay) {
    if (!validate(tx))
        return;
    if (delay != sc_core::SC_ZERO_TIME) {
        sc_core::wait(delay);
        delay = sc_core::SC_ZERO_TIME;
    }
    if (!reset_n.read()) {
        tx.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
        return;
    }
    const auto generation = reset_generation_;
    bus_lock_.lock();
    // A queued caller must not survive a reset pulse while the mutex is held,
    // even if reset has already been released before it acquires the bus.
    if (!reset_n.read() || generation != reset_generation_)
        tx.set_response_status(tlm::TLM_GENERIC_ERROR_RESPONSE);
    else {
        pending_ = &tx;
        sc_core::wait(completed_);
        // Allow the single output driver to publish the clocked state.
        sc_core::wait(sc_core::SC_ZERO_TIME);
    }
    bus_lock_.unlock();
}
unsigned Gpio::transport_dbg(tlm::tlm_generic_payload& tx) {
    if (!validate(tx))
        return 0;
    // Debug reads are non-destructive; debug writes are deliberately unsupported.
    if (tx.is_write()) {
        tx.set_response_status(tlm::TLM_COMMAND_ERROR_RESPONSE);
        return 0;
    }
    transfer(tx, false);
    return 4;
}
} // namespace fu3::gpio
