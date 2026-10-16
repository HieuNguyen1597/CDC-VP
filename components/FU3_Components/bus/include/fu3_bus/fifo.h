#pragma once
#include <systemc>
#include <algorithm>
#include <deque>

namespace fu3_bus {
// Cooperative SystemC queue: an owner cannot overtake existing waiters.
class Fifo {
    sc_core::sc_event changed_;
    std::deque<const void*> queue_;
public:
    class Lease {
        Fifo& fifo_;
        void release() {
            auto& q = fifo_.queue_;
            const auto it = std::find(q.begin(), q.end(), this);
            if (it != q.end()) q.erase(it);
            fifo_.changed_.notify(sc_core::SC_ZERO_TIME);
        }
    public:
        explicit Lease(Fifo& fifo) : fifo_(fifo) {
            fifo_.queue_.push_back(this);
            try {
                while (fifo_.queue_.front() != this) sc_core::wait(fifo_.changed_);
            } catch (...) { release(); throw; }
        }
        ~Lease() { release(); }
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;
    };
};
}
