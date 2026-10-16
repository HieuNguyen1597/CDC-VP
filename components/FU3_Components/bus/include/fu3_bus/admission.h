#pragma once
#include <systemc>
#include <algorithm>
#include <deque>

namespace fu3_bus {
// FIFO admission with bounded occupancy. Cancellation releases queue and capacity.
class Admission {
    sc_core::sc_event changed_;
    std::deque<const void*> queue_;
    unsigned active_ = 0, capacity_ = 1, peak_ = 0;
public:
    void capacity(unsigned n) { capacity_ = n; }
    unsigned active() const { return active_; }
    unsigned peak() const { return peak_; }
    class Lease {
        Admission& gate_;
        bool admitted_ = false;
        void release() {
            auto& q=gate_.queue_;
            auto it=std::find(q.begin(),q.end(),this);
            if(it!=q.end()) q.erase(it);
            if(admitted_) { --gate_.active_; admitted_=false; }
            gate_.changed_.notify(sc_core::SC_ZERO_TIME);
        }
    public:
        explicit Lease(Admission& gate) : gate_(gate) {
            gate_.queue_.push_back(this);
            try {
                while(gate_.queue_.front()!=this || gate_.active_>=gate_.capacity_)
                    sc_core::wait(gate_.changed_);
                gate_.queue_.pop_front(); ++gate_.active_; admitted_=true;
                gate_.peak_=std::max(gate_.peak_,gate_.active_);
                gate_.changed_.notify(sc_core::SC_ZERO_TIME);
            } catch(...) { release(); throw; }
        }
        ~Lease() { release(); }
        Lease(const Lease&)=delete;
        Lease& operator=(const Lease&)=delete;
    };
};
}
