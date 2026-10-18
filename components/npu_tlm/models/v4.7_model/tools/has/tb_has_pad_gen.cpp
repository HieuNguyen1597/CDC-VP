// tb_has_pad_gen.cpp -- unit test of has/has_pad_gen.h (padding generator of the Data Flow Controller).
//
// For random layers (image size, kernel, stride, padding on each side, tile size, PAD_TAIL) and every tile of the tile
// iterator, the SRAM-A content is built the way the Data Flow Controller stages it (lead + real part) and every
// element the feeder would gather is addressed with the registers of the generator:
//     address = c * CHSTEP + wy * YSTEP + wx,   valid = ROW_LO <= wy * YSTEP < ROW_HI  and  COL_LO <= wx < COL_HI
// (wy, wx = row / column inside the tile's input window). The value, or the padding value when not valid, must equal
// the image element at that position, or the padding value outside the image. This checks the formulas, not the
// cycle behaviour of the feeder (tools/has/tb_has_npu_top with HAS_FEEDER_PAD=1 does that).
//   g++ -std=c++17 -O2 -I. tools/has/tb_has_pad_gen.cpp -o tb_has_pad_gen
#include <cstdio>
#include <random>
#include <vector>
#include "has/has_pad_gen.h"
#include "has/has_tile_iter.h"

using namespace has;

int main()
{
    std::mt19937 rng(12345);
    auto rnd = [&](int lo, int hi) { return int(std::uniform_int_distribution<int>(lo, hi)(rng)); };
    long layers = 0, tiles = 0, padded = 0, elems = 0, bad = 0, off_aligned = 0, off_none = 0, fills_read = 0;
    const uint8_t fill = 0x5A;
    for (int it = 0; it < 4000; it++)
    {
        const int cin = rnd(1, 5), in_h = rnd(1, 40), in_w = rnd(1, 40);
        const int kh = rnd(1, 7), kw = rnd(1, 7), s = rnd(1, 3);
        const int pt = rnd(0, 4), pl = rnd(0, 4), pb = rnd(0, 4), pr = rnd(0, 4);
        if (in_h + pt + pb < kh || in_w + pl + pr < kw) continue;
        LayerGeom g;
        g.cout = 4; g.oh = (in_h + pt + pb - kh) / s + 1; g.ow = (in_w + pl + pr - kw) / s + 1;
        g.kh = kh; g.kw = kw; g.sy = g.sx = s; g.pad_t = pt; g.pad_l = pl; g.pad_b = pb; g.pad_r = pr;
        g.cout_t = 4; g.h_t = rnd(1, g.oh); g.w_t = rnd(1, g.ow);
        g.pad_tail = rnd(0, 3) == 0;
        const int pad_val = rnd(-128, 127);
        std::vector<int8_t> img(size_t(cin) * in_h * in_w);
        for (auto &v : img) v = int8_t(rnd(-128, 127));
        layers++;
        TileIter ti(g);
        for (long i = 0; i < ti.count(); i++)
        {
            const TileGeom t = ti.at(i);
            tiles++;
            const PadGen p = PadGen::make(in_h, in_w, t.iy0, t.iy1, t.ix0, t.ix1, kh, kw, s, pad_val);
            const bool has_pad = t.iy0 < 0 || t.ix0 < 0 || t.iy1 > in_h || t.ix1 > in_w;
            if (!p.on)
            {
                // off is only allowed for: no padding, the aligned 1x1 mode, or a window entirely outside the image
                const bool empty = std::min(t.iy1, in_h) <= std::max(t.iy0, 0) || std::min(t.ix1, in_w) <= std::max(t.ix0, 0);
                if (!has_pad) off_none++;
                else if (kh == 1 && kw == 1 && s == 1) off_aligned++;
                else if (!empty) { bad++; if (bad <= 5) std::printf("  FAIL: generator off on a padded tile (layer %d tile %ld)\n", it, i); }
                continue;
            }
            padded++;
            // SRAM-A as staged: lead, then [cin][hr][wr]
            std::vector<uint8_t> mem(size_t(p.bytes(cin)), fill);
            for (int c = 0; c < cin; c++)
                for (uint32_t y = 0; y < p.hr; y++)
                    for (uint32_t x = 0; x < p.wr; x++)
                        mem[p.lead + (size_t(c) * p.hr + y) * p.wr + x] =
                            uint8_t(img[(size_t(c) * in_h + size_t(t.iy0 + int(p.pt) + int(y))) * in_w + size_t(t.ix0 + int(p.pl) + int(x))]);
            const uint64_t ystep = p.ystep(), chstep = p.chstep();
            const uint32_t col_lo = p.reg_col() & 0xFFFFu, col_hi = p.reg_col() >> 16, row_lo = p.reg_row_lo(), row_hi = p.reg_row_hi();
            if (p.ylim(kh) != ystep * uint64_t(kh) || p.chlim(cin) != chstep * uint64_t(cin) ||
                p.til_ylim(s, t.oy1 - t.oy0) != p.til_ystep(s) * uint64_t(t.oy1 - t.oy0) || (p.reg_ctrl() & 1u) == 0 ||
                int(int8_t((p.reg_ctrl() >> 8) & 0xFFu)) != pad_val)
            { bad++; if (bad <= 5) std::printf("  FAIL: register values (layer %d tile %ld)\n", it, i); }
            const int ah = t.iy1 - t.iy0, aw = t.ix1 - t.ix0;
            // the feeder forms the row from two counters: output row x TIL_YSTEP + kernel row x YSTEP
            for (int oyr = 0; oyr < t.oy1 - t.oy0; oyr++)
                for (int ky = 0; ky < kh; ky++)
                    if (uint64_t(oyr) * p.til_ystep(s) + uint64_t(ky) * ystep != uint64_t(oyr * s + ky) * ystep)
                    { bad++; if (bad <= 5) std::printf("  FAIL: TIL_YSTEP / YSTEP do not add up to the window row (layer %d tile %ld)\n", it, i); }
            if (p.til_ylim(s, t.oy1 - t.oy0) > (uint64_t(1) << 18) - 1 + p.til_ystep(s))
            { bad++; if (bad <= 5) std::printf("  FAIL: TIL_YLIM beyond the index counter (layer %d tile %ld)\n", it, i); }
            for (int c = 0; c < cin; c++)
                for (int wy = 0; wy < ah; wy++)
                    for (int wx = 0; wx < aw; wx++)
                    {
                        const uint64_t addr = uint64_t(c) * chstep + uint64_t(wy) * ystep + uint64_t(wx);
                        const uint64_t row = uint64_t(wy) * ystep;
                        const bool valid = row >= row_lo && row < row_hi && uint32_t(wx) >= col_lo && uint32_t(wx) < col_hi;
                        const int iy = t.iy0 + wy, ix = t.ix0 + wx;
                        const bool inside = iy >= 0 && iy < in_h && ix >= 0 && ix < in_w;
                        const int want = inside ? int(img[(size_t(c) * in_h + size_t(iy)) * in_w + size_t(ix)]) : pad_val;
                        int got = pad_val;
                        if (valid)
                        {
                            if (addr >= mem.size()) { bad++; if (bad <= 5) std::printf("  FAIL: valid address beyond the data (layer %d tile %ld)\n", it, i); continue; }
                            if (addr < p.lead) fills_read++;
                            got = int(int8_t(mem[size_t(addr)]));
                        }
                        elems++;
                        if (got != want || valid != inside)
                        {
                            bad++;
                            if (bad <= 5)
                                std::printf("  FAIL: layer %d tile %ld c %d wy %d wx %d: got %d want %d valid %d inside %d (pt %u pl %u hr %u wr %u)\n",
                                            it, i, c, wy, wx, got, want, int(valid), int(inside), p.pt, p.pl, p.hr, p.wr);
                        }
                    }
        }
    }
    // a result that does not fit its register turns the generator off: one condition at a time
    {
        // columns only: 70,001 columns, 2 rows (rows x width = 140,000 fits the 18-bit index)
        const PadGen wide = PadGen::make(2, 70000, 0, 2, -1, 70000, 1, 3, 2, 0);
        if (wide.on || wide.fits) { bad++; std::printf("  FAIL: a 70000-column window must not fit the 16-bit column fields\n"); }
        // ROW_HI only: 300 columns (fit), (pt + hr) x wr = 1000 x 300 beyond 2^18 - 1
        const PadGen tall = PadGen::make(999, 300, -1, 999, 0, 300, 3, 3, 1, 0);
        if (tall.on || tall.fits) { bad++; std::printf("  FAIL: ROW_HI of 300,000 must not fit the 18-bit index\n"); }
        // last row tag only: ROW_HI = 800 x 300 = 240,000 fits, the window of 900 rows does not (bottom padding)
        const PadGen deep = PadGen::make(800, 300, 0, 900, 0, 300, 3, 3, 1, 0);
        if (deep.on || deep.fits) { bad++; std::printf("  FAIL: a row tag of 269,700 must not fit the 18-bit index\n"); }
        // and a tile at the limits that does fit
        const PadGen ok = PadGen::make(800, 300, -1, 800, 0, 300, 3, 3, 1, 0);
        if (!ok.on) { bad++; std::printf("  FAIL: a tile inside every limit must be accepted\n"); }
    }
    std::printf("[tb_has_pad_gen] layers %ld, tiles %ld (padded by the generator %ld, off: no padding %ld, aligned 1x1 %ld), "
                "window elements checked %ld, lead elements read as data %ld\n", layers, tiles, padded, off_none, off_aligned, elems, fills_read);
    // Dilation (separate random stream, so the counts above stay comparable): the feeder reads window row
    // output row x stride + kernel row x dilation, formed by TIL_YSTEP and YSTEP; every element it reads is checked.
    long dlayers = 0, dtiles = 0, dpadded = 0, delems = 0, dfills = 0;
    {
        std::mt19937 rng2(952);
        auto rn = [&](int lo, int hi) { return int(std::uniform_int_distribution<int>(lo, hi)(rng2)); };
        for (int it = 0; it < 3000; it++)
        {
            const int cin = rn(1, 3), in_h = rn(1, 40), in_w = rn(1, 40);
            const int kh = rn(1, 5), kw = rn(1, 5), s = rn(1, 2), d = rn(2, 9);
            if ((kw - 1) * d > 63) continue;
            const int ekh = (kh - 1) * d + 1, ekw = (kw - 1) * d + 1;
            const int pt = rn(0, std::min(ekh - 1, 12)), pl = rn(0, std::min(ekw - 1, 12));
            const int pb = rn(0, std::min(ekh - 1, 12)), pr = rn(0, std::min(ekw - 1, 12));
            if (in_h + pt + pb < ekh || in_w + pl + pr < ekw) continue;
            LayerGeom g;
            g.cout = 4; g.oh = (in_h + pt + pb - ekh) / s + 1; g.ow = (in_w + pl + pr - ekw) / s + 1;
            g.kh = kh; g.kw = kw; g.sy = g.sx = s; g.dil = d; g.pad_t = pt; g.pad_l = pl; g.pad_b = pb; g.pad_r = pr;
            g.cout_t = 4; g.h_t = rn(1, g.oh); g.w_t = rn(1, g.ow);
            g.pad_tail = rn(0, 3) == 0;
            const int pad_val = rn(-128, 127);
            std::vector<int8_t> img(size_t(cin) * in_h * in_w);
            for (auto &v : img) v = int8_t(rn(-128, 127));
            dlayers++;
            TileIter ti(g);
            for (long i = 0; i < ti.count(); i++)
            {
                const TileGeom t = ti.at(i);
                dtiles++;
                if (t.iy1 - t.iy0 != (t.oy1 - t.oy0 - 1) * s + ekh || t.ix1 - t.ix0 != (t.ox1 - t.ox0 - 1) * s + ekw)
                { bad++; if (bad <= 5) std::printf("  FAIL: dilation window size (layer %d tile %ld)\n", it, i); }
                const PadGen p = PadGen::make(in_h, in_w, t.iy0, t.iy1, t.ix0, t.ix1, kh, kw, s, pad_val, 18, d);
                const bool has_pad = t.iy0 < 0 || t.ix0 < 0 || t.iy1 > in_h || t.ix1 > in_w;
                const bool empty = std::min(t.iy1, in_h) <= std::max(t.iy0, 0) || std::min(t.ix1, in_w) <= std::max(t.ix0, 0);
                if (!p.on)
                {
                    if (has_pad && !empty) { bad++; if (bad <= 5) std::printf("  FAIL: dilation: generator off on a padded tile (layer %d tile %ld)\n", it, i); }
                    continue;
                }
                dpadded++;
                std::vector<uint8_t> mem(size_t(p.bytes(cin)), fill);
                for (int c = 0; c < cin; c++)
                    for (uint32_t y = 0; y < p.hr; y++)
                        for (uint32_t x = 0; x < p.wr; x++)
                            mem[p.lead + (size_t(c) * p.hr + y) * p.wr + x] =
                                uint8_t(img[(size_t(c) * in_h + size_t(t.iy0 + int(p.pt) + int(y))) * in_w + size_t(t.ix0 + int(p.pl) + int(x))]);
                const uint64_t ystep = p.ystep(), chstep = p.chstep(), tys = p.til_ystep(s);
                const uint32_t col_lo = p.reg_col() & 0xFFFFu, col_hi = p.reg_col() >> 16, row_lo = p.reg_row_lo(), row_hi = p.reg_row_hi();
                // the Y counter takes exactly kh values 0, YSTEP, ..., (kh - 1) x YSTEP below YLIM
                if (ystep != uint64_t(p.wr) * uint64_t(d) || !(uint64_t(kh - 1) * ystep < p.ylim(kh)) || p.ylim(kh) > uint64_t(kh) * ystep)
                { bad++; if (bad <= 5) std::printf("  FAIL: dilation YSTEP / YLIM (layer %d tile %ld)\n", it, i); }
                for (int c = 0; c < cin; c++)
                    for (int oyr = 0; oyr < t.oy1 - t.oy0; oyr++)
                        for (int ky = 0; ky < kh; ky++)
                            for (int wx = 0; wx < t.ix1 - t.ix0; wx++)
                            {
                                const uint64_t row = uint64_t(oyr) * tys + uint64_t(ky) * ystep;   // tag = window row x wr
                                const int wy = oyr * s + ky * d;
                                const uint64_t addr = uint64_t(c) * chstep + row + uint64_t(wx);
                                const bool valid = row >= row_lo && row < row_hi && uint32_t(wx) >= col_lo && uint32_t(wx) < col_hi;
                                const int iy = t.iy0 + wy, ix = t.ix0 + wx;
                                const bool inside = iy >= 0 && iy < in_h && ix >= 0 && ix < in_w;
                                const int want = inside ? int(img[(size_t(c) * in_h + size_t(iy)) * in_w + size_t(ix)]) : pad_val;
                                int got = pad_val;
                                if (valid)
                                {
                                    if (addr >= mem.size()) { bad++; if (bad <= 5) std::printf("  FAIL: dilation: valid address beyond the data (layer %d tile %ld)\n", it, i); continue; }
                                    if (addr < p.lead) dfills++;
                                    got = int(int8_t(mem[size_t(addr)]));
                                }
                                delems++;
                                if (got != want || valid != inside)
                                {
                                    bad++;
                                    if (bad <= 5)
                                        std::printf("  FAIL: dilation layer %d tile %ld c %d wy %d wx %d d %d: got %d want %d valid %d inside %d\n",
                                                    it, i, c, wy, wx, d, got, want, int(valid), int(inside));
                                }
                            }
            }
        }
    }
    std::printf("[tb_has_pad_gen] dilation 2 to 9: layers %ld, tiles %ld (padded by the generator %ld), elements read by the feeder "
                "checked %ld, lead elements read as data %ld\n", dlayers, dtiles, dpadded, delems, dfills);
    const bool pass = bad == 0 && fills_read == 0 && padded > 1000 && dfills == 0 && dpadded > 500;
    std::printf("[tb_has_pad_gen] RESULT: %s (%ld failed checks)\n", pass ? "PASS" : "FAIL", bad);
    return pass ? 0 : 1;
}
