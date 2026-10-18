// rtl_ref_gvu_vrf.h -- register storage of the vector unit, ported line by line from the RTL (neo_core/gvu/stream_switch):
// vrf.sv, vstream_reg_int32.sv, vstream_reg_int64_type0.sv, vstream_reg_int64_type1.sv, stream_vrf.sv. Pure C++17, no
// SystemC. The structural model of the same registers is has/gvu_vrf.h (has::vrf_fits); nothing here is used by the
// instruction path.
// Two different organisations exist in the RTL:
//   Vrf        vrf.sv: DEPTH (32) rows of 8 * NUM_LANE (256) bits = 1 KB, read / written as v32int8 (1 row), v32int16
//              (2 rows), v32int32 (4 rows) or v32int64 (8 rows) through 2 / 2 / 3 / 2 ports; combinational read, clocked write.
//   StreamVrf  stream_vrf.sv: three v32int64 and four v32int32 valid / ready registers = 1.25 KB; int64 #0 splits into
//              4 x v32int16, int64 #2 into 2 x v32int32, int32 #0 into 2 x v32int16 or 4 x v32int8.
// A vector is a byte array, least significant byte first: lane l of a v32intN vector occupies bytes [l*N/8, (l+1)*N/8).
#ifndef RTL_REF_GVU_VRF_H
#define RTL_REF_GVU_VRF_H

#include <array>
#include <cstdint>
#include <vector>
#include "has/rtl_ref_gvu_fu.h"

namespace rtl_ref_gvu
{
    typedef std::vector<uint8_t> Bytes;

    // ---- vrf.sv --------------------------------------------------------------------------------------------------------
    struct VrfParams
    {
        int NUM_LANE{32}, NUM_INT8_PORT{2}, NUM_INT16_PORT{2}, NUM_INT32_PORT{3}, NUM_INT64_PORT{2}, DEPTH{32};
        int row_bytes() const { return NUM_LANE; }                       // 8 * NUM_LANE bits
        int bytes() const { return DEPTH * NUM_LANE; }
    };
    class Vrf
    {
    public:
        struct Write { bool wen{false}; unsigned waddr{0}; Bytes wdata; };
        // One clock edge: the write ports of each width, index = port number.
        struct Writes { std::vector<Write> int8, int16, int32, int64; };
        explicit Vrf(const VrfParams &p = VrfParams()) : P_(p), mem_(static_cast<size_t>(p.bytes()), 0) {}
        const VrfParams &params() const { return P_; }
        // gen_vrf_r_proc: rows_per = 1 / 2 / 4 / 8; the address counts vectors of that width (row = rows_per * raddr + i).
        Bytes read(int rows_per, unsigned raddr) const
        {
            Bytes d(static_cast<size_t>(rows_per * P_.row_bytes()), 0);
            raddr &= static_cast<unsigned>(P_.DEPTH / rows_per - 1);     // INTn_AW bits
            for (int i = 0; i < rows_per; i++)
                for (int b = 0; b < P_.row_bytes(); b++)
                    d[static_cast<size_t>(i * P_.row_bytes() + b)] = mem_[static_cast<size_t>((rows_per * static_cast<int>(raddr) + i) * P_.row_bytes() + b)];
            return d;
        }
        // gen_vrf_wr_proc: non-blocking assignments in the order int8 ports, int16, int32, int64 (ascending port number
        // inside each), so when two enabled ports cover the same row the later one in that order wins.
        void tick(const Writes &w)
        {
            std::vector<uint8_t> next = mem_;
            apply(next, w.int8, 1, P_.NUM_INT8_PORT);
            apply(next, w.int16, 2, P_.NUM_INT16_PORT);
            apply(next, w.int32, 4, P_.NUM_INT32_PORT);
            apply(next, w.int64, 8, P_.NUM_INT64_PORT);
            mem_.swap(next);
        }

    private:
        VrfParams P_;
        std::vector<uint8_t> mem_;
        void apply(std::vector<uint8_t> &next, const std::vector<Write> &ports, int rows_per, int nports) const
        {
            for (int j = 0; j < nports && j < static_cast<int>(ports.size()); j++)
            {
                const Write &q = ports[static_cast<size_t>(j)];
                if (!q.wen) continue;
                const unsigned a = q.waddr & static_cast<unsigned>(P_.DEPTH / rows_per - 1);
                for (int i = 0; i < rows_per * P_.row_bytes(); i++)
                    next[static_cast<size_t>(rows_per * static_cast<int>(a) * P_.row_bytes() + i)] = q.wdata[static_cast<size_t>(i)];
            }
        }
    };

    // ---- vstream_reg_int32.sv, vstream_reg_int64_type0.sv, vstream_reg_int64_type1.sv ----------------------------------
    // N pipeline_reg of `sub` bytes each, used as one register of the full width or as N (or N/2) narrower registers:
    //   INT32     4 x 32 B   i_cfg_mode 2: 4 x v32int8; 1: 2 x v32int16; 0 and 3: one v32int32
    //   INT64_T0  4 x 64 B   i_cfg_mode != 0: 4 x v32int16; 0: one v32int64
    //   INT64_T1  2 x 128 B  i_cfg_mode != 0: 2 x v32int32; 0: one v32int64
    // The data outputs of every view always show the register contents; valid and ready are 0 on the views not selected.
    class VstreamReg
    {
    public:
        enum Kind { INT32, INT64_T0, INT64_T1 };
        struct Port { bool flush{false}, valid{false}, ready{false}; Bytes data; };   // ready: the consumer side input
        struct In { unsigned i_cfg_mode{0}; Port full; std::array<Port, 2> half; std::array<Port, 4> sub; };
        struct PortOut { bool o_ready{false}, o_valid{false}; Bytes o_data; };
        struct Out { PortOut full; std::array<PortOut, 2> half; std::array<PortOut, 4> sub; };

        explicit VstreamReg(Kind k)
            : kind_(k), n_(k == INT64_T1 ? 2 : 4), sub_(k == INT32 ? 32 : (k == INT64_T0 ? 64 : 128))
        {
            for (int i = 0; i < n_; i++) reg_[static_cast<size_t>(i)].data.assign(static_cast<size_t>(sub_), 0);
        }
        int subregs() const { return n_; }
        int sub_bytes() const { return sub_; }

        Out eval(const In &in) const { Arb a; return comb(in, a); }
        void tick(const In &in, bool i_rstn = true)
        {
            Arb a;
            comb(in, a);
            for (int i = 0; i < n_; i++)
                reg_[static_cast<size_t>(i)].tick(i_rstn, a.flush[i], a.valid_in[i], a.data_in[static_cast<size_t>(i)], a.ready_in[i]);
        }

    private:
        enum Sel { FULL, HALF, SUB };
        struct Arb { bool flush[4], valid_in[4], ready_in[4]; std::array<Bytes, 4> data_in; };
        Kind kind_;
        int n_, sub_;
        std::array<PipelineReg<Bytes>, 4> reg_;

        Sel in_sel(unsigned m) const { return kind_ == INT32 ? (m == 2 ? SUB : (m == 1 ? HALF : FULL)) : (m != 0 ? SUB : FULL); }
        static Bytes slice(const Bytes &d, int off, int len)
        {
            Bytes r(static_cast<size_t>(len), 0);
            for (int i = 0; i < len && off + i < static_cast<int>(d.size()); i++) r[static_cast<size_t>(i)] = d[static_cast<size_t>(off + i)];
            return r;
        }
        Out comb(const In &in, Arb &a) const
        {
            const Sel s = in_sel(in.i_cfg_mode & 3u);
            // pipe_reg_in_arb
            for (int i = 0; i < n_; i++)
            {
                const Port &src = s == SUB ? in.sub[static_cast<size_t>(i)] : (s == HALF ? in.half[static_cast<size_t>(i / 2)] : in.full);
                a.flush[i] = src.flush;
                a.valid_in[i] = src.valid;
                a.data_in[static_cast<size_t>(i)] = s == SUB ? slice(src.data, 0, sub_) : (s == HALF ? slice(src.data, (i % 2) * sub_, sub_) : slice(src.data, i * sub_, sub_));
            }
            // pipe_reg_out_arb: the selected view supplies the ready inputs of the registers
            for (int i = 0; i < n_; i++)
                a.ready_in[i] = s == SUB ? in.sub[static_cast<size_t>(i)].ready
                              : (s == HALF ? in.half[static_cast<size_t>(i / 2)].ready : in.full.ready);
            Out o;
            bool rdy[4], vld[4];
            Bytes all;
            for (int i = 0; i < n_; i++)
            {
                const PipelineReg<Bytes> &r = reg_[static_cast<size_t>(i)];
                rdy[i] = r.o_ready(a.valid_in[i], a.ready_in[i]);
                vld[i] = r.valid;
                all.insert(all.end(), r.data.begin(), r.data.end());
            }
            o.full.o_data = all;
            o.full.o_ready = s == FULL && rdy[0];
            o.full.o_valid = s == FULL && vld[0];
            for (int k = 0; k < 2; k++)
            {
                o.half[static_cast<size_t>(k)].o_data = kind_ == INT32 ? slice(all, 2 * k * sub_, 2 * sub_) : Bytes();
                o.half[static_cast<size_t>(k)].o_ready = s == HALF && rdy[2 * k];
                o.half[static_cast<size_t>(k)].o_valid = s == HALF && vld[2 * k];
            }
            for (int i = 0; i < n_; i++)
            {
                o.sub[static_cast<size_t>(i)].o_data = slice(all, i * sub_, sub_);
                o.sub[static_cast<size_t>(i)].o_ready = s == SUB && rdy[i];
                o.sub[static_cast<size_t>(i)].o_valid = s == SUB && vld[i];
            }
            return o;
        }
    };

    // ---- stream_vrf.sv -------------------------------------------------------------------------------------------------
    // Port map of the top level (index = port number of stream_vrf):
    //   vint64[0] = int64_0 full, vint64[1] = int64_1 (plain pipeline_reg), vint64[2] = int64_2 full
    //   vint32[0] = int32_0 full, vint32[1..3] = int32_1..3 (plain pipeline_reg), vint32[4..5] = int64_2 sub 0..1
    //   vint16[0..3] = int64_0 sub 0..3, vint16[4..5] = int32_0 half 0..1
    //   vint8[0..3]  = int32_0 sub 0..3
    // Config: i_vint64_0_cfg_mode and i_vint64_2_cfg_mode are 1 bit (zero-extended to the 2-bit port of the register),
    // i_vint32_0_cfg_mode is 2 bits.
    struct StreamVrf
    {
        static constexpr int kVint64Ports = 3, kVint32Ports = 6, kVint16Ports = 6, kVint8Ports = 4;
        static constexpr int kBytes = 3 * 256 + 4 * 128;                 // 1280 B
        typedef VstreamReg::Port Port;
        typedef VstreamReg::PortOut PortOut;
        struct In
        {
            unsigned i_vint32_0_cfg_mode{0};
            bool i_vint64_0_cfg_mode{false}, i_vint64_2_cfg_mode{false};
            std::array<Port, 3> vint64; std::array<Port, 6> vint32, vint16; std::array<Port, 4> vint8;
        };
        struct Out { std::array<PortOut, 3> vint64; std::array<PortOut, 6> vint32, vint16; std::array<PortOut, 4> vint8; };

        VstreamReg int64_0{VstreamReg::INT64_T0}, int64_2{VstreamReg::INT64_T1}, int32_0{VstreamReg::INT32};
        PipelineReg<Bytes> int64_1;
        std::array<PipelineReg<Bytes>, 3> int32_1_3;
        StreamVrf() { int64_1.data.assign(256, 0); for (auto &r : int32_1_3) r.data.assign(128, 0); }

        Out eval(const In &i) const
        {
            Out o;
            const VstreamReg::Out a = int64_0.eval(in_int64_0(i)), c = int64_2.eval(in_int64_2(i)), d = int32_0.eval(in_int32_0(i));
            o.vint64[0] = a.full; o.vint64[1] = plain(int64_1, i.vint64[1]); o.vint64[2] = c.full;
            o.vint32[0] = d.full;
            for (int k = 1; k < 4; k++) o.vint32[static_cast<size_t>(k)] = plain(int32_1_3[static_cast<size_t>(k - 1)], i.vint32[static_cast<size_t>(k)]);
            o.vint32[4] = c.sub[0]; o.vint32[5] = c.sub[1];
            for (int k = 0; k < 4; k++) o.vint16[static_cast<size_t>(k)] = a.sub[static_cast<size_t>(k)];
            o.vint16[4] = d.half[0]; o.vint16[5] = d.half[1];
            for (int k = 0; k < 4; k++) o.vint8[static_cast<size_t>(k)] = d.sub[static_cast<size_t>(k)];
            return o;
        }
        void tick(const In &i, bool i_rstn = true)
        {
            int64_0.tick(in_int64_0(i), i_rstn); int64_2.tick(in_int64_2(i), i_rstn); int32_0.tick(in_int32_0(i), i_rstn);
            int64_1.tick(i_rstn, i.vint64[1].flush, i.vint64[1].valid, i.vint64[1].data, i.vint64[1].ready);
            for (int k = 1; k < 4; k++)
            {
                const Port &q = i.vint32[static_cast<size_t>(k)];
                int32_1_3[static_cast<size_t>(k - 1)].tick(i_rstn, q.flush, q.valid, q.data, q.ready);
            }
        }

    private:
        static PortOut plain(const PipelineReg<Bytes> &r, const Port &q) { PortOut o; o.o_ready = r.o_ready(q.valid, q.ready); o.o_valid = r.valid; o.o_data = r.data; return o; }
        static VstreamReg::In in_int64_0(const In &i)
        {
            VstreamReg::In r; r.i_cfg_mode = i.i_vint64_0_cfg_mode ? 1u : 0u; r.full = i.vint64[0];
            for (int k = 0; k < 4; k++) r.sub[static_cast<size_t>(k)] = i.vint16[static_cast<size_t>(k)];
            return r;
        }
        static VstreamReg::In in_int64_2(const In &i)
        {
            VstreamReg::In r; r.i_cfg_mode = i.i_vint64_2_cfg_mode ? 1u : 0u; r.full = i.vint64[2];
            r.sub[0] = i.vint32[4]; r.sub[1] = i.vint32[5];
            return r;
        }
        static VstreamReg::In in_int32_0(const In &i)
        {
            VstreamReg::In r; r.i_cfg_mode = i.i_vint32_0_cfg_mode & 3u; r.full = i.vint32[0];
            r.half[0] = i.vint16[4]; r.half[1] = i.vint16[5];
            for (int k = 0; k < 4; k++) r.sub[static_cast<size_t>(k)] = i.vint8[static_cast<size_t>(k)];
            return r;
        }
    };
} // namespace rtl_ref_gvu

#endif // RTL_REF_GVU_VRF_H
