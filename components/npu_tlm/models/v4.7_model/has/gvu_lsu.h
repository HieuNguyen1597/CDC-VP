// gvu_lsu.h -- vector-unit load/store unit (vector-unit specification table 1 "Load/store unit": address generator modes
// v32int32, v32int8, int32, int8 for the multi-bank Scratchpad SRAM; section 4, table 3 bank organisation).
// Accounting model: the pipelines keep computing on their own data; each stage reports the Scratchpad transfers it makes
// (bank, mode, element count), and the LSU counts transfers (one per vector or scalar access) and bytes per bank and mode.
// The counts back question H8 (Scratchpad capacity and ports) with data; they do not change results or timing.
//
// MAS V1.0 "Vector Load/Store Unit (VLSU)" and table "Scratchpad SRAM banks & ports organization" add:
//   - a v32int16 logical port (vbank#1 port#1, LayerNorm stages 3-4)                         -> LsuMode::V32I16
//   - the logical ports of each bank                                                         -> lsu_port()
//   - an address generator (AGU) per port with a 4-D stride mode: every dimension has its own stride and repetition
//     count, so reshape, slice, split and concat only change the generated addresses         -> GvuAgu
//   - a direct write of the output stream to the system bus through the AXI4 master of the core -> store_axi()
// Padding and transpose are marked "to be defined" in the MAS (transpose is planned on the PSUM stream); GvuAgu::permuted()
// gives the read order a permuted tensor needs, which the 4-D stride mode can already express.
#ifndef HAS_GVU_LSU_H
#define HAS_GVU_LSU_H

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace has
{
    enum class LsuBank { SBANK = 0, VBANK0 = 1, VBANK1 = 2, VBANK2 = 3 };
    enum class LsuMode { V32I32 = 0, V32I8 = 1, S32 = 2, S8 = 3, V32I16 = 4 };

    // Logical port of `b` that carries mode `m` (MAS table "Scratchpad SRAM banks & ports organization"), or -1 when the
    // bank has no such port: sbank port#0 int32 / port#1 int8; vbank#0 v32int8; vbank#1 port#0 v32int32 / port#1 v32int16 /
    // port#2 v32int8; vbank#2 port#0 v32int32 / port#1 v32int8.
    inline int lsu_port(LsuBank b, LsuMode m)
    {
        switch (b)
        {
        case LsuBank::SBANK: return m == LsuMode::S32 ? 0 : (m == LsuMode::S8 ? 1 : -1);
        case LsuBank::VBANK0: return m == LsuMode::V32I8 ? 0 : -1;
        case LsuBank::VBANK1: return m == LsuMode::V32I32 ? 0 : (m == LsuMode::V32I16 ? 1 : (m == LsuMode::V32I8 ? 2 : -1));
        case LsuBank::VBANK2: return m == LsuMode::V32I32 ? 0 : (m == LsuMode::V32I8 ? 1 : -1);
        }
        return -1;
    }
    // Physical ports: the sbank is dual-port (both logical ports usable in the same cycle), every vbank is single-port
    // (its logical ports share one physical port through data-selection muxes).
    inline bool lsu_dual_port(LsuBank b) { return b == LsuBank::SBANK; }
    inline const char *lsu_bank_name(LsuBank b)
    {
        static const char *n[] = {"sbank", "vbank#0", "vbank#1", "vbank#2"};
        return n[int(b)];
    }
    inline const char *lsu_mode_name(LsuMode m)
    {
        static const char *n[] = {"v32int32", "v32int8", "int32", "int8", "v32int16"};
        return n[int(m)];
    }
    inline uint32_t lsu_elem_bytes(LsuMode m)
    {
        return (m == LsuMode::V32I32 || m == LsuMode::S32) ? 4u : (m == LsuMode::V32I16 ? 2u : 1u);
    }

    class GvuLsu
    {
    public:
        static constexpr int LANES = 32, MODES = 5;
        struct Cnt { uint64_t rd{0}, wr{0}, rd_bytes{0}, wr_bytes{0}; };

        // `elems` elements moved in mode `m`: vector modes move LANES elements per access, scalar modes one.
        void load(LsuBank b, LsuMode m, uint64_t elems) { acc(b, m, elems, false); }
        void store(LsuBank b, LsuMode m, uint64_t elems) { acc(b, m, elems, true); }
        // Output stream written straight to the system bus (AXI4 master of the core) instead of a Scratchpad bank.
        void store_axi(uint64_t bytes) { axi_wr++; axi_wr_bytes += bytes; }

        const Cnt &at(LsuBank b, LsuMode m) const { return c_[int(b)][int(m)]; }
        uint64_t accesses() const
        {
            uint64_t n = 0;
            for (auto &row : c_) for (auto &x : row) n += x.rd + x.wr;
            return n;
        }
        // Transfers reported on a (bank, mode) pair the bank has no logical port for.
        uint64_t illegal() const { return illegal_; }
        uint64_t axi_wr{0}, axi_wr_bytes{0};
        void reset() { *this = GvuLsu(); }

        // One line per bank / mode that was used: "vbank#0 v32int8 rd 120 (3840 B) wr 40 (1280 B)".
        std::string report() const
        {
            std::string s;
            char line[160];
            for (int b = 0; b < 4; b++)
                for (int m = 0; m < MODES; m++)
                {
                    const Cnt &x = c_[b][m];
                    if (!x.rd && !x.wr) continue;
                    std::snprintf(line, sizeof line, "%s %s rd %llu (%llu B) wr %llu (%llu B)\n", lsu_bank_name(LsuBank(b)),
                                  lsu_mode_name(LsuMode(m)), (unsigned long long)x.rd, (unsigned long long)x.rd_bytes,
                                  (unsigned long long)x.wr, (unsigned long long)x.wr_bytes);
                    s += line;
                }
            return s;
        }

    private:
        Cnt c_[4][MODES];
        uint64_t illegal_{0};
        void acc(LsuBank b, LsuMode m, uint64_t elems, bool wr)
        {
            const bool vec = m == LsuMode::V32I32 || m == LsuMode::V32I8 || m == LsuMode::V32I16;
            const uint64_t esz = lsu_elem_bytes(m);
            const uint64_t n = vec ? (elems + LANES - 1) / LANES : elems;
            if (lsu_port(b, m) < 0) illegal_++;
            Cnt &x = c_[int(b)][int(m)];
            if (wr) { x.wr += n; x.wr_bytes += elems * esz; }
            else { x.rd += n; x.rd_bytes += elems * esz; }
        }
    };

    // Address generator of one Scratchpad port, 4-D stride mode (MAS "Vector Load/Store Unit"): address = base +
    // sum_k i_k * stride_k, i_k in [0, count_k), dimension 0 outermost and dimension 3 innermost (fastest). Addresses are in
    // elements of the port's mode. The builders give the configuration of each tensor operation of the MAS; none of them
    // moves data, they only change the order or the subset of addresses.
    struct GvuAgu
    {
        uint32_t base{0};
        uint32_t count[4]{1, 1, 1, 1};
        int64_t stride[4]{0, 0, 0, 0};

        uint64_t size() const { return uint64_t(count[0]) * count[1] * count[2] * count[3]; }
        int64_t addr(uint32_t i0, uint32_t i1, uint32_t i2, uint32_t i3) const
        {
            return int64_t(base) + i0 * stride[0] + i1 * stride[1] + i2 * stride[2] + i3 * stride[3];
        }
        std::vector<int64_t> sequence() const
        {
            std::vector<int64_t> s;
            s.reserve(static_cast<size_t>(size()));
            for (uint32_t a = 0; a < count[0]; a++)
                for (uint32_t b = 0; b < count[1]; b++)
                    for (uint32_t c = 0; c < count[2]; c++)
                        for (uint32_t d = 0; d < count[3]; d++) s.push_back(addr(a, b, c, d));
            return s;
        }

        // A tensor of `shape` stored row-major at `base_addr` (reshape keeps this order: only `shape` changes).
        static GvuAgu tensor(uint32_t base_addr, const uint32_t shape[4])
        {
            GvuAgu g;
            g.base = base_addr;
            int64_t st = 1;
            for (int k = 3; k >= 0; k--) { g.count[k] = shape[k]; g.stride[k] = st; st *= shape[k]; }
            return g;
        }
        // Transpose / permute: output dimension k walks input dimension perm[k].
        GvuAgu permuted(const int perm[4]) const
        {
            GvuAgu g;
            g.base = base;
            for (int k = 0; k < 4; k++) { g.count[k] = count[perm[k]]; g.stride[k] = stride[perm[k]]; }
            return g;
        }
        // Slice along `axis`: elements offset, offset + step, ... below `end`. Split = one slice with step 1 per part.
        GvuAgu sliced(int axis, uint32_t offset, uint32_t end, uint32_t step) const
        {
            GvuAgu g = *this;
            g.base = static_cast<uint32_t>(int64_t(base) + int64_t(offset) * stride[axis]);
            g.count[axis] = end > offset ? (end - offset + step - 1) / step : 0;
            g.stride[axis] = stride[axis] * int64_t(step);
            return g;
        }
        // Concat: the addresses one input of length `part_len` along `axis` occupies inside the output tensor, when it
        // starts at `offset` along that axis.
        static GvuAgu concat_part(uint32_t base_addr, const uint32_t out_shape[4], int axis, uint32_t offset, uint32_t part_len)
        {
            return tensor(base_addr, out_shape).sliced(axis, offset, offset + part_len, 1);
        }
    };
} // namespace has

#endif // HAS_GVU_LSU_H
