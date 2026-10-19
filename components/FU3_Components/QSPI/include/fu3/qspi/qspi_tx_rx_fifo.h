#pragma once
#include <cstdint>
#include <deque>
#include <stdexcept>

namespace fu3::qspi {
enum class ThresholdUnit { Bytes, Words };

class QspiTxRxFifo {
public:
    class Queue {
    public:
        explicit Queue(unsigned capacity) : capacity_(capacity) {
            if (!capacity || capacity>32) throw std::invalid_argument("QSPI FIFO capacity must be 1..32 bytes");
        }
        unsigned size() const { return static_cast<unsigned>(data_.size()); }
        unsigned free() const { return capacity_-size(); }
        unsigned capacity() const { return capacity_; }
        bool empty() const { return data_.empty(); }
        unsigned char operator[](unsigned i) const { return data_.at(i); }
        unsigned char front() const { return data_.at(0); }
        void pop_front() { if (empty()) throw std::underflow_error("QSPI FIFO empty"); data_.pop_front(); }
        void push_back(unsigned char value) {
            if (!free()) throw std::overflow_error("QSPI FIFO full");
            data_.push_back(value);
        }
        void clear() { data_.clear(); }
        bool push_word(std::uint32_t value,unsigned bytes) {
            if (!bytes || bytes>4 || bytes>free()) return false;
            for (unsigned i=0;i<bytes;++i) push_back(static_cast<unsigned char>(value>>(8*i)));
            return true;
        }
    private:
        unsigned capacity_;
        std::deque<unsigned char> data_;
    };
    Queue tx,rx;
    explicit QspiTxRxFifo(unsigned tx_bytes=32,unsigned rx_bytes=32,ThresholdUnit unit=ThresholdUnit::Bytes)
        : tx(tx_bytes),rx(rx_bytes),unit_(unit) {
        if (unit!=ThresholdUnit::Bytes && unit!=ThresholdUnit::Words)
            throw std::invalid_argument("Unknown QSPI threshold unit");
    }
    void reset() { tx.clear(); rx.clear(); }
    bool tx_threshold(unsigned encoded) const { return tx.free()>=(encoded+1)*(unit_==ThresholdUnit::Words?4:1); }
    bool rx_threshold(unsigned encoded) const {
        return (unit_==ThresholdUnit::Words ? (rx.size()+3)/4 : rx.size())>=encoded+1;
    }
    std::uint32_t levels() const { return (tx.size()<<8)|(rx.size()<<16); }
private:
    ThresholdUnit unit_;
};
}
