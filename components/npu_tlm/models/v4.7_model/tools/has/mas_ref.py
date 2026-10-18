#!/usr/bin/env python3
"""Reference vectors for the vector-unit items added from the MAS (tools/has/tb_gvu_mas.cpp reads them).

Written from the MAS text and figures only (Vector Round-Shift Unit, Vector LUT Unit, Softmax pipeline); it shares no
code with has/gvu_*.h or tools/fe/fe_ref_gvu.py.

usage: python3 tools/has/mas_ref.py <out dir>

Files (one case per line, '#' lines are comments):
  round_ceil.txt          p s out                       ceil(p / 2^s)
  round_trunc.txt         p s out                       p / 2^s rounded toward zero
  round_away.txt          p s out                       p / 2^s rounded away from zero
  lut_ls_recip.txt        'T' line = 256 table entries; rows  x idx r      log-scale index, Base 7, table 2^21 / x
  lut_ls_rsqrt.txt        'T' line = 512 table entries; rows  x idx r      log-scale index, Base 0, table 2^14 / sqrt(x)
  lut_pwl_mas_recip.txt   rows  x r E      PWL mode with the 256-entry table (last entry held)
  lut_pwl_mas_rsqrt.txt   rows  x r E      PWL mode with the 512-entry table (last entry of the odd segment held)
  softmax_ls.txt          'E' line = exp table (256), 'T' line = recip table (256);
                          rows  L | x_0..x_L-1 | m sigma r | a_0..a_L-1     log-scale recip, Base 7, E_shift 7
"""
import math
import os
import random
import sys

Q = 14
FINE = 4


def rnd(v):
    """Round half away from zero (v >= 0 here)."""
    return int(math.floor(v + 0.5))


def msb(x):
    return x.bit_length() - 1


def ceil_shift(p, s):
    return -((-p) >> s)


def trunc_shift(p, s):
    q = abs(p) >> s
    return -q if p < 0 else q


def away_shift(p, s):
    q = -((-abs(p)) >> s)
    return -q if p < 0 else q


def half_up(p, s):
    return (p + (1 << (s - 1))) >> s if s > 0 else p


# ---------------------------------------------------------------- log-scale index
def ls_table(rsqrt, base, q_total):
    n = 1 << ((5 if rsqrt else 4) + FINE)
    t = []
    for i in range(n):
        seg, f = i >> FINE, i & 15
        xm = (1.0 + (f + 0.5) / 16.0) * 2.0 ** (base + seg)
        v = 2.0 ** q_total / (math.sqrt(xm) if rsqrt else xm)
        t.append(32767 if v >= 32767.0 else rnd(v))
    return t


def ls_index(x, rsqrt, base):
    if x == 0:
        return 0
    e = msb(x)
    seg = (e - base) & ((1 << (5 if rsqrt else 4)) - 1)
    fine = ((x >> (e - FINE)) & 15) if e >= FINE else 0   # RTL: no fine bits below 16
    return (seg << FINE) | fine


# ---------------------------------------------------------------- PWL mode, MAS table depths
def pwl_tables():
    recip = [rnd(2.0 ** Q / (1.0 + i / 256.0)) for i in range(256)]
    rsqrt = [rnd(2.0 ** Q / math.sqrt((1.0 + i / 256.0) * (1 << p))) for p in range(2) for i in range(256)]
    return recip, rsqrt


def pwl_mas(x, rsqrt, recip_t, rsqrt_t):
    e = msb(x)
    y = (x << (31 - e)) & 0xFFFFFFFF
    i, frac = (y >> 23) & 0xFF, (y >> 15) & 0xFF
    if rsqrt:
        a = ((e & 1) << 8) | i
        t0 = rsqrt_t[a]
        t1 = rsqrt_t[a + 1] if a + 1 < 512 else t0
        return t0 + (((t1 - t0) * frac) >> 8), e >> 1
    t0 = recip_t[i]
    t1 = recip_t[i + 1] if i + 1 < 256 else t0
    return t0 + (((t1 - t0) * frac) >> 8), e


# ---------------------------------------------------------------- softmax with the log-scale recip
def requant(x, S, s, zp):
    r = half_up(x * S, s)
    r = max(-32768, min(32767, r))
    return max(-128, min(127, r + zp))


def softmax_ls(xs, exp_t, recip_t, base, e_shift):
    m = max(xs)
    e = [exp_t[m - x] for x in xs]
    sigma = sum(e)
    r = recip_t[ls_index(sigma, False, base)]
    sh = e_shift + Q - 15
    return m, sigma, r, [requant(half_up(r * ej, sh), 1 << 30, 37, -128) for ej in e]


def sample_x(rng):
    k = rng.randrange(4)
    if k == 0:
        return rng.randrange(1, 1 << 12)
    if k == 1:
        return rng.randrange(1, 1 << 20)
    if k == 2:
        return 1 << rng.randrange(0, 31)
    return rng.randrange(1, 1 << 31)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "mas_vec"
    os.makedirs(out, exist_ok=True)
    rng = random.Random(20261)

    with open(os.path.join(out, "round_ceil.txt"), "w") as f:
        f.write("# p s ceil(p / 2^s)\n")
        for p, s in [(0, 1), (1, 1), (-1, 1), (3, 1), (-3, 1), (4, 2), (5, 2), (-5, 2), (-4, 2), (7, 3), (-7, 3), (-8, 3), (-9, 3)]:
            f.write("%d %d %d\n" % (p, s, ceil_shift(p, s)))
        for _ in range(4000):
            p, s = rng.randrange(-(1 << 62), 1 << 62), rng.randrange(1, 63)
            f.write("%d %d %d\n" % (p, s, ceil_shift(p, s)))

    for fn, name in ((trunc_shift, "round_trunc.txt"), (away_shift, "round_away.txt")):
        with open(os.path.join(out, name), "w") as f:
            f.write("# p s out\n")
            for p, s in [(0, 1), (1, 1), (-1, 1), (3, 1), (-3, 1), (4, 2), (5, 2), (-5, 2), (-4, 2), (7, 3), (-7, 3), (-8, 3), (-9, 3)]:
                f.write("%d %d %d\n" % (p, s, fn(p, s)))
            for _ in range(4000):
                p, s = rng.randrange(-(1 << 62), 1 << 62), rng.randrange(1, 63)
                f.write("%d %d %d\n" % (p, s, fn(p, s)))

    for rsqrt, base, qt, name in ((False, 7, Q + 7, "lut_ls_recip.txt"), (True, 0, Q, "lut_ls_rsqrt.txt")):
        t = ls_table(rsqrt, base, qt)
        with open(os.path.join(out, name), "w") as f:
            f.write("# log-scale index, %s, Base %d, q_total %d\nT %s\n" % ("rsqrt" if rsqrt else "recip", base, qt, " ".join(map(str, t))))
            xs = list(range(1, 3000)) + [sample_x(rng) for _ in range(20000)]
            for x in xs:
                i = ls_index(x, rsqrt, base)
                f.write("%d %d %d\n" % (x, i, t[i]))

    recip_t, rsqrt_t = pwl_tables()
    for rsqrt, name in ((False, "lut_pwl_mas_recip.txt"), (True, "lut_pwl_mas_rsqrt.txt")):
        with open(os.path.join(out, name), "w") as f:
            f.write("# PWL mode, MAS table depth (%d entries), last entry held\n" % (512 if rsqrt else 256))
            xs = list(range(1, 3000)) + [sample_x(rng) for _ in range(20000)]
            xs += [(511 << k) | ((1 << k) - 1) for k in range(0, 22)]   # mantissa index 255, every exponent
            for x in xs:
                r, e = pwl_mas(x, rsqrt, recip_t, rsqrt_t)
                f.write("%d %d %d\n" % (x, r, e))

    exp_t = [rnd(255 * math.exp(-d * 0.0625)) for d in range(256)]
    t = ls_table(False, 7, Q + 7)
    with open(os.path.join(out, "softmax_ls.txt"), "w") as f:
        f.write("# softmax, log-scale recip (Base 7, E_shift 7), exp = round(255 * exp(-d / 16))\n")
        f.write("E %s\nT %s\n" % (" ".join(map(str, exp_t)), " ".join(map(str, t))))
        for n in range(400):
            L = (8, 32, 64, 197)[n % 4]
            spread = (10, 40, 127)[n % 3]
            xs = [max(-128, min(127, int(rng.gauss(0, spread)))) for _ in range(L)]
            m, sigma, r, a = softmax_ls(xs, exp_t, t, 7, 7)
            f.write("%d | %s | %d %d %d | %s\n" % (L, " ".join(map(str, xs)), m, sigma, r, " ".join(map(str, a))))
    print("wrote reference vectors to %s" % out)


if __name__ == "__main__":
    main()
