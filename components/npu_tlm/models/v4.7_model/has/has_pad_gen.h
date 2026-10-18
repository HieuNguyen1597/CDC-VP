// has_pad_gen.h -- padding generator of the Data Flow Controller for the on-the-fly padding of the IFMAP feeder.
//
// With feeder padding (HAS_FEEDER_PAD, data_feeder/rtl_ref_ifmap_feeder_rtl.h) SRAM-A holds only the part of a tile's
// input window that lies inside the image, and the feeder replaces every other element by the padding value while it
// gathers. This block derives, for one tile, everything that flow needs:
//   - what the DMA loads: rows [iy0 + pt, iy0 + pt + hr) x columns [ix0 + pl, ix0 + pl + wr) of each channel, written
//     to SRAM-A behind `lead` unused elements;
//   - the four padding registers of the core (ACT_PAD_CTRL / COL / ROW_LO / ROW_HI, rtl_ref_config_map.h);
//   - the six activation-counter fields that depend on the row width (they step with the real width wr and height hr
//     instead of the padded ones).
// Why the lead: the feeder addresses element (channel c, window row wy, window column wx) at
// c * wr * hr + wy * wr + wx. With lead = pt * wr + pl the real element (wy - pt, wx - pl) of a channel sits exactly
// there, so the address counters need no offset and never go below zero.
//
// Hardware view: comparators and subtractors on the window bounds, then five multiplications (pt * wr, hr * wr,
// (hr * wr) * cin, wr * kh, wr * stride and (wr * stride) * tile height share operands; widths in the comments). It is
// combinational and, like every other per-tile configuration value of this model, costs no cycle; `fits` is false when
// a result does not fit its register, and the Data Flow Controller then keeps the staged flow for that tile.
//
// Pure C++ (no SystemC), tested alone by tools/has/tb_has_pad_gen.cpp.
#ifndef HAS_PAD_GEN_H
#define HAS_PAD_GEN_H

#include <algorithm>
#include <cstdint>

namespace has
{
    struct PadGen
    {
        static constexpr int COL_W = 16;   // width of the two column fields of ACT_PAD_COL

        bool on{false};      // false: no padding in this tile, aligned 1x1 address mode, nothing inside the image, or !fits
        bool outside{false}; // the input window of the tile reaches outside the image (whatever `on` says)
        bool fits{true};
        uint32_t pt{0}, pl{0};   // padding rows above / columns left of the real part, inside this tile's window
        uint32_t hr{0}, wr{0};   // real part: rows x columns
        uint32_t lead{0};        // pt * wr + pl: unused elements in front of the real part in SRAM-A
        uint32_t d{1};           // dilation of the layer
        int val{0};              // padding value (int8)

        // iy0..ix1: input window of the tile in image coordinates, half open, may reach outside [0, in_h) x [0, in_w)
        // (has::TileGeom, already sized with the effective kernel (k - 1) * dil + 1). kh / kw / stride / dil: kernel,
        // stride and dilation of the layer. idx_w: width of the activation index counters of the core (SAURIA_ACT_IDX_W).
        // The mask of the feeder compares element positions, so dilation changes only the row step of the counters.
        static PadGen make(int in_h, int in_w, int iy0, int iy1, int ix0, int ix1, int kh, int kw, int stride, int pad_val,
                           int idx_w = 18, int dil = 1)
        {
            PadGen p;
            p.val = pad_val;
            p.d = uint32_t(std::max(dil, 1));
            p.outside = iy0 < 0 || ix0 < 0 || iy1 > in_h || ix1 > in_w;
            // 1x1 stride-1 layers use the aligned address mode of the feeder (one SRAM word per group of positions,
            // sauria_compute_core_fields); it needs rows that are a multiple of the word width, which wr is not.
            if (kh == 1 && kw == 1 && stride == 1 && p.d == 1) return p;
            const int y0 = std::max(iy0, 0), y1 = std::min(iy1, in_h), x0 = std::max(ix0, 0), x1 = std::min(ix1, in_w);
            if (y1 <= y0 || x1 <= x0) return p;                                   // nothing of the window is inside the image
            if (y0 == iy0 && y1 == iy1 && x0 == ix0 && x1 == ix1) return p;       // no padding in this tile
            p.pt = uint32_t(y0 - iy0); p.pl = uint32_t(x0 - ix0);
            p.hr = uint32_t(y1 - y0); p.wr = uint32_t(x1 - x0);
            p.lead = p.pt * p.wr + p.pl;
            const uint64_t col_max = (uint64_t(1) << COL_W) - 1, idx_max = (uint64_t(1) << idx_w) - 1;
            // columns: both fields of ACT_PAD_COL; rows: ROW_HI and the largest row tag the feeder compares, the last
            // window row (iy1 - iy0 - 1) x wr -- a tag that wrapped could fall back inside the valid range
            p.fits = uint64_t(p.pl) + p.wr <= col_max && uint64_t(p.pt + p.hr) * p.wr <= idx_max &&
                     uint64_t(iy1 - iy0) * p.wr <= idx_max;
            p.on = p.fits;
            return p;
        }

        // ---- padding registers (valid when on) ----
        uint32_t reg_ctrl() const { return 1u | (uint32_t(uint8_t(int8_t(val))) << 8); }    // bit 0 enable, bits 15:8 value
        uint32_t reg_col() const { return (pl & 0xFFFFu) | ((pl + wr) << COL_W); }           // valid columns [pl, pl + wr)
        uint32_t reg_row_lo() const { return pt * wr; }                                      // valid rows, x row step wr
        uint32_t reg_row_hi() const { return (pt + hr) * wr; }

        // ---- activation-counter fields that follow the row width (as sauria_compute_core_fields with A_w = wr) ----
        // cin: input channels of the core pass; h_til: output rows of the tile.
        uint64_t ylim(int kh) const { return uint64_t(wr) * (uint64_t(kh - 1) * d + 1); }
        uint64_t ystep() const { return uint64_t(wr) * d; }
        uint64_t chstep() const { return uint64_t(wr) * hr; }
        uint64_t chlim(int cin) const { return uint64_t(wr) * hr * uint64_t(cin); }
        uint64_t til_ystep(int stride) const { return uint64_t(wr) * uint64_t(stride); }
        uint64_t til_ylim(int stride, int h_til) const { return uint64_t(h_til) * wr * uint64_t(stride); }

        // Elements of SRAM-A the tile occupies with `cin` channels.
        uint64_t bytes(int cin) const { return uint64_t(lead) + uint64_t(cin) * hr * wr; }
    };
} // namespace has

#endif
