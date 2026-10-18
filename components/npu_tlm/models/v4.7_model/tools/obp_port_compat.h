// obp_port_compat.h -- drives the two ports the native Obp (psm/obp_top.h) gained: i_channel_idx and i_output_zp.
//
// The Obp used to count the channel of a vector itself: the counter was read on every clock edge with i_valid high and
// then incremented, and it returned to 0 on an edge without i_valid once the three pipeline stages were empty. The Obp
// now takes the channel from the PSM side band (Psm::o_channel_idx). Testbenches that feed the Obp from a source without
// that side band (the RTL-accurate PSM, or vectors written by the testbench) attach this module: it reproduces the
// former counter, modulo `mod` (the former Y_DIM), one clock ahead on a signal, so the Obp reads at each edge the value
// the internal counter had at that edge. The output zero point is held at 0 (the former behaviour: none added).
//
// usage, inside the constructor of the testbench module, after the Obp is created:
//     (new sauria::ObpPortCompat("obp_compat", 32))->attach(obp, i_clk, rstn, obp_in_valid);
#ifndef SAURIA_OBP_PORT_COMPAT_H
#define SAURIA_OBP_PORT_COMPAT_H

#include <systemc.h>
#include <cstdint>

namespace sauria
{
    struct ObpPortCompat : sc_core::sc_module
    {
        sc_core::sc_in<bool> i_clk{"i_clk"};
        sc_core::sc_in<bool> i_rstn{"i_rstn"};
        sc_core::sc_in<bool> i_valid{"i_valid"};
        sc_core::sc_signal<uint32_t> s_channel_idx{"s_channel_idx"};
        sc_core::sc_signal<int32_t> s_output_zp{"s_output_zp"};

        SC_HAS_PROCESS(ObpPortCompat);
        ObpPortCompat(sc_core::sc_module_name n, uint32_t mod) : sc_core::sc_module(n), mod_(mod ? mod : 1)
        {
            SC_METHOD(count);
            sensitive << i_clk.pos();
        }

        template <class ObpT, class Clk, class Rst, class Valid>
        void attach(ObpT *obp, Clk &clk, Rst &rstn, Valid &valid)
        {
            i_clk(clk);
            i_rstn(rstn);
            i_valid(valid);
            obp->i_channel_idx(s_channel_idx);
            obp->i_output_zp(s_output_zp);
        }

    private:
        void count()
        {
            if (!i_rstn.read())
            {
                cnt_ = 0;
                v1_ = v2_ = v3_ = false;
                s_channel_idx.write(0);
                return;
            }
            const bool v = i_valid.read();
            if (v) cnt_++;
            else if (!v1_ && !v2_ && !v3_) cnt_ = 0;   // the three Obp stages are empty
            v3_ = v2_;
            v2_ = v1_;
            v1_ = v;
            s_channel_idx.write(cnt_ % mod_);
        }

        uint32_t mod_;
        uint32_t cnt_{0};
        bool v1_{false}, v2_{false}, v3_{false};
    };
} // namespace sauria

#endif // SAURIA_OBP_PORT_COMPAT_H
