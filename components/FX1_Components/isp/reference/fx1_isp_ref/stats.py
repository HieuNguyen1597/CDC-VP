"""Frame-based reference of the FX1 ISP statistics generators and of their
software-visible publication: AEC (HAS §6.21), AWB (§6.22), AF (§6.23), §9.5.

The measurement functions work on whole frames with numpy; `StatsState`
holds what the register interface shows (frame counter, AEC shadow/active
sets, published results, the AEC zone/histogram memory and the two AWB result
banks with their ownership) and answers reads of the statistics registers.
Rules follow plan/alg/ALG_F (ALG-AEC-*, ALG-AWB-*, ALG-AF-*, ALG-STAT-*),
DEC-29, DEC-30 and the CSR/MAS revision of 2026-10-07: AWB result banks with
acquire/release (MAS §6.22.4), AF enable sampled at SOF (MAS §6.23.2),
untouched AEC zones read green minimum 4095 (MAS §6.21.4.3).

The reference is frame based: it is read only at frame boundaries, where the
single AEC zone/histogram memory (DEC-30) holds the last enabled frame
complete. Software commands (AWB acquire/release) arrive between frames, so
the same-edge eligibility rules of MAS §6.22.4.3 do not arise here; they are
checked by the C++ directed tests.
The partial sums visible during a frame are checked by the C++ directed tests
(tests/unit/test_stats.cpp) and the TLM probe in test_e2e_tlm.cpp.
"""

import numpy as np

I64 = np.int64


# ---------------------------------------------------------------- measurement
def aec_measure(p, c):
    """p: post-BPC RGGB Bayer [H, W]; c: active AEC set (dict)."""
    h, w = p.shape
    yy, xx = np.mgrid[0:h, 0:w]
    ch = ((yy & 1) << 1) | (xx & 1)                       # 0 R, 1 Gr, 2 Gb, 3 B
    q = (p >= c["min"]) & (p <= c["max"])                 # clip, inclusive
    green = (ch == 1) | (ch == 2)
    res = {
        "gsum": [int(p[q & (ch == k)].sum()) for k in range(4)],
        "gcnt": [int((q & (ch == k)).sum()) for k in range(4)],
        "hist": np.bincount((p[green] >> 6).ravel(), minlength=64).tolist(),
        "nx": c["nx"], "ny": c["ny"],
        "zones": 1 <= c["nx"] <= 32 and 1 <= c["ny"] <= 24 and c["zw"] > 0 and c["zh"] > 0,
    }
    if res["zones"]:
        n = c["nx"] * c["ny"]
        z = np.minimum(yy // c["zh"], c["ny"] - 1) * c["nx"] + np.minimum(xx // c["zw"], c["nx"] - 1)
        zsum = np.zeros((n, 4), I64)
        zcnt = np.zeros((n, 4), I64)
        for k in range(4):
            m = q & (ch == k)
            np.add.at(zsum[:, k], z[m], p[m])
            np.add.at(zcnt[:, k], z[m], 1)
        zg, pg = z[green], p[green]
        gmin = np.full(n, 4095, I64)
        gmax = np.zeros(n, I64)
        np.minimum.at(gmin, zg, pg)
        np.maximum.at(gmax, zg, pg)
        res.update(zsum=zsum.tolist(), zcnt=zcnt.tolist(),
                   oe=np.bincount(zg[pg > c["th_oe"]], minlength=n).tolist(),
                   ue=np.bincount(zg[pg < c["th_ue"]], minlength=n).tolist(),
                   gmin=gmin.tolist(), gmax=gmax.tolist())
    return res


def awb_measure(rgb, c):
    """rgb: post-Demosaic [H, W, 3]; c: AWB set sampled at SOF (dict)."""
    h, w, _ = rgb.shape
    ok = np.all((rgb > c["under"]) & (rgb < c["sat"]), axis=-1)   # strict at both limits
    res = {"sum": [int(rgb[..., k][ok].sum()) for k in range(3)], "n": int(ok.sum()),
           "nx": c["nx"], "ny": c["ny"], "zones": 1 <= c["nx"] <= 64 and 1 <= c["ny"] <= 32}
    if res["zones"]:
        nx, ny = c["nx"], c["ny"]
        zx = np.minimum(((np.arange(w, dtype=I64) + 1) * nx - 1) // w, nx - 1)
        zy = np.minimum(((np.arange(h, dtype=I64) + 1) * ny - 1) // h, ny - 1)
        z = (zy[:, None] * nx + zx[None, :])[ok]
        zsum = np.zeros((nx * ny, 3), I64)
        for k in range(3):
            np.add.at(zsum[:, k], z, rgb[..., k][ok])
        res.update(zsum=zsum.tolist(), zcnt=np.bincount(z, minlength=nx * ny).tolist())
    return res


def af_measure(y):
    """y: CSC luma [H, W]: Sobel |gx| + |gy|, replicate borders, 4x4 zones."""
    h, w = y.shape
    e = np.pad(y.astype(I64), 1, mode="edge")
    def s(dy, dx):
        return e[1 + dy:1 + dy + h, 1 + dx:1 + dx + w]
    gx = (s(-1, 1) + 2 * s(0, 1) + s(1, 1)) - (s(-1, -1) + 2 * s(0, -1) + s(1, -1))
    gy = (s(1, -1) + 2 * s(1, 0) + s(1, 1)) - (s(-1, -1) + 2 * s(-1, 0) + s(-1, 1))
    d = np.abs(gx) + np.abs(gy)
    z = (np.arange(h)[:, None] * 4 // h) * 4 + (np.arange(w)[None, :] * 4 // w)
    fv = np.zeros(16, I64)
    np.add.at(fv, z, d)
    return [int(v) & 0xFFFFFFFF for v in fv]


# ---------------------------------------------------------------- register view
class StatsState:
    def __init__(self):
        self.counter = 0                # frames started since reset (ALG-STAT-01)
        self.fid = 0
        self.aec_commit = False
        self.aec_active = {"en": 0, "nx": 32, "ny": 24, "zw": 120, "zh": 90, "min": 0, "max": 4095,
                           "th_ue": 256, "th_oe": 3840}
        self.awb_sof = None
        self.af_sof = 0
        self.ctx = {}
        self.aec = None                 # published results
        self.af = [0] * 16
        self.aec_live = False           # memory restarted by an enabled SOF since reset
        self.ro = {}                    # published register values
        self._awb_clear()

    def _awb_clear(self):
        """AWB ownership and reporting after a hard or COMMON soft reset (MAS p248)."""
        self.awb_banks = [None, None]   # None = FREE, else {"res", "fid", "ctx", "state"}
        self.awb_ready = []             # READY banks, completion order
        self.awb_writing = None
        self.awb_leased = None
        self.awb_overrun = False
        self.awb_drops = 0

    def soft_reset(self):
        """COMMON soft reset (MAS Table 183, ALG-STAT-07): results, done bits and
        context tags cleared, zones untouched again; AWB ownership and reporting
        cleared; a pending AEC commit is kept (owner Q4, DEC-32 superseded);
        FRAME_IDs, the counter and the active configuration are kept."""
        self.aec = None
        self.af = [0] * 16
        self.aec_live = False
        self.ro = {k: v for k, v in self.ro.items() if k.endswith("_FRAME_ID")}
        self._awb_clear()

    def abort(self):
        """A frame abandoned after its SOF: the AWB measurement in flight frees
        its bank; READY and LEASED banks survive (MAS §6.22.5.4-5)."""
        if self.awb_writing is not None:
            self.awb_banks[self.awb_writing] = None
            self.awb_writing = None

    def _awb_loss(self):
        self.awb_overrun = True
        self.awb_drops = min(self.awb_drops + 1, 0xFFFFFFFF)

    def _awb_admit(self, regs):
        """Reserve a FREE bank at an enabled SOF, bank 0 first; an illegal grid
        or no FREE bank skips the measurement and reports it (MAS §6.22.3.7,
        §6.22.5.5)."""
        self.awb_writing = None
        c = self.awb_sof
        if not c["en"]:
            return
        w, h = regs.active_size()
        legal = (1 <= w <= 3840 and 1 <= h <= 2160 and 1 <= c["nx"] <= min(64, w)
                 and 1 <= c["ny"] <= min(32, h))
        free = [b for b in (0, 1) if self.awb_banks[b] is None]
        if not legal or not free:
            self._awb_loss()
            c["en"] = 0
            return
        b = free[0]
        self.awb_banks[b] = {"state": "writing", "res": None, "fid": self.fid, "ctx": self.ctx["AWB"]}
        self.awb_writing = b

    def awb_command(self, value):
        """AWB_RESULT_CTRL (MAS Table 138): CLEAR_REPORT, then RELEASE (wins
        over ACQUIRE), else ACQUIRE of the oldest READY bank."""
        if value & 4:
            self.awb_overrun = False
            self.awb_drops = 0
        if value & 2:
            if self.awb_leased is not None:
                self.awb_banks[self.awb_leased] = None
                self.awb_leased = None
        elif value & 1:
            if self.awb_leased is None and self.awb_ready:
                self.awb_leased = self.awb_ready.pop(0)
                self.awb_banks[self.awb_leased]["state"] = "leased"

    def _awb_lease(self):
        return self.awb_banks[self.awb_leased] if self.awb_leased is not None else None

    def sof(self, regs):
        self.counter = (self.counter + 1) & 0xFFFFFFFF
        self.fid = self.counter
        if self.aec_commit:             # shadow -> active (#296)
            self.aec_active = {
                "en": regs.f("AEC_CTRL", "en"),
                "nx": regs.f("AEC_ZONE_CFG", "num_zone_x"), "ny": regs.f("AEC_ZONE_CFG", "num_zone_y"),
                "zw": regs.f("AEC_ZONE_SIZE", "zone_width"), "zh": regs.f("AEC_ZONE_SIZE", "zone_height"),
                "min": regs.f("AEC_SAMPLE_CLIP", "sample_min_clip"),
                "max": regs.f("AEC_SAMPLE_CLIP", "sample_max_clip"),
                "th_ue": regs.f("AEC_THRESH", "th_ue"), "th_oe": regs.f("AEC_THRESH", "th_oe")}
            self.aec_commit = False
        self.awb_sof = {"en": regs.f("AWB_CTRL", "en"), "nx": regs.f("AWB_CTRL", "num_zone_x"),
                        "ny": regs.f("AWB_CTRL", "num_zone_y"),
                        "under": regs.f("AWB_UNDEREXPOSED_LIMIT", "underexposed_limit"),
                        "sat": regs.f("AWB_SATURATION_LIMIT", "saturation_limit")}
        self.af_sof = regs.f("AF_CTRL", "en")
        self.ctx = {b: regs.value[b + "_CONTEXT_ID"] for b in ("AEC", "AWB", "AF")}
        if self.aec_active["en"]:
            self.aec_live = True        # single memory restarted (DEC-30)
        self._awb_admit(regs)

    def eof(self, regs, taps):
        if self.aec_active["en"]:
            self.aec = aec_measure(taps["aec"], self.aec_active)
            self._publish("AEC", "AEC_STATUS", 0x2)
        if self.awb_writing is not None:    # the complete bank becomes READY
            bank = self.awb_banks[self.awb_writing]
            bank["res"] = awb_measure(taps["awb"], self.awb_sof)
            bank["state"] = "ready"
            self.awb_ready.append(self.awb_writing)
            self.awb_writing = None
            self.ro["AWB_STATUS"] = self.ro.get("AWB_STATUS", 0) | 0x1
        if self.af_sof:                 # EN sampled at SOF (MAS §6.23.2)
            self.af = af_measure(taps["af"])
            self.ro["AF_STATUS"] = self.ro.get("AF_STATUS", 0) | 0x20   # score_valid: published scores
            self._publish("AF", "AF_STATUS", 0x2)

    def _publish(self, blk, status, done):
        self.ro[blk + "_FRAME_ID"] = self.fid
        self.ro[blk + "_RESULT_CONTEXT_ID"] = self.ctx[blk]
        self.ro[status] = self.ro.get(status, 0) | done

    # Reads of the statistics registers, as software sees them after the frame.
    def read(self, regs, name):
        sel = regs.f("AEC_CHANNEL_SEL", "channel_sel")
        a = self.aec
        if name in ("AEC_GLOBAL_SUM_LO", "AEC_GLOBAL_SUM_HI", "AEC_GLOBAL_COUNT"):
            if a is None:
                return 0
            return {"AEC_GLOBAL_SUM_LO": a["gsum"][sel] & 0xFFFFFFFF,
                    "AEC_GLOBAL_SUM_HI": (a["gsum"][sel] >> 32) & 1,
                    "AEC_GLOBAL_COUNT": a["gcnt"][sel] & 0x1FFFFF}[name]
        if name.startswith("AEC_ZONE_") and name != "AEC_ZONE_ADDR":
            z = regs.f("AEC_ZONE_ADDR", "zone_addr")
            if not self.aec_live or a is None:   # untouched since reset (MAS §6.21.4.3)
                return 4095 if name == "AEC_ZONE_GREEN_MIN_MAX" else 0
            inside = a["zones"] and z < a["nx"] * a["ny"]
            if name == "AEC_ZONE_GREEN_MIN_MAX":
                return (a["gmax"][z] << 12) | a["gmin"][z] if inside else 4095
            if not inside:
                return 0
            return {"AEC_ZONE_SUM": a["zsum"][z][sel] & 0x0FFFFFFF,
                    "AEC_ZONE_COUNT": a["zcnt"][z][sel] & 0xFFFF,
                    "AEC_ZONE_GREEN_OE_UE": ((a["oe"][z] & 0xFFFF) << 16) | (a["ue"][z] & 0xFFFF)}[name]
        if name == "AEC_HIST_DATA":
            return a["hist"][regs.f("AEC_HIST_ADDR", "hist_addr")] & 0x3FFFFF if a and self.aec_live else 0
        lease = self._awb_lease()
        if name == "AWB_RESULT_STATUS":
            v = len(self.awb_ready) | (0x40 if self.awb_overrun else 0) | (0x20 if self.awb_writing is not None else 0)
            if lease:
                b = lease["res"]
                v |= 0xC | (self.awb_leased << 4) | (b["nx"] << 8) | (b["ny"] << 16)
            return v
        if name == "AWB_DROP_COUNT":
            return self.awb_drops
        if name == "AWB_RESULT_CTRL":
            return 0                    # commands read zero
        if name in ("AWB_FRAME_ID", "AWB_RESULT_CONTEXT_ID", "AWB_GLOBAL_COUNT") or name.startswith("AWB_GLOBAL_SUM_"):
            if not lease:
                return 0                # reads without a lease return zero
            b = lease["res"]
            if name == "AWB_FRAME_ID":
                return lease["fid"]
            if name == "AWB_RESULT_CONTEXT_ID":
                return lease["ctx"]
            if name == "AWB_GLOBAL_COUNT":
                return b["n"] & 0x7FFFFF
            v = b["sum"]["RGB".index(name[15])]
            return (v >> 32) & 7 if name.endswith("_H") else v & 0xFFFFFFFF
        if name.startswith("AWB_ZONE_") and name != "AWB_ZONE_ADDR":
            b = lease["res"] if lease else None
            z = regs.f("AWB_ZONE_ADDR", "zone_addr")
            if not (b and b["zones"] and z < b["nx"] * b["ny"]):
                return 0
            if name == "AWB_ZONE_COUNT":
                return b["zcnt"][z] & 0x7FFFFF
            v = b["zsum"][z]["RGB".index(name[13])]
            return (v >> 32) & 7 if name.endswith("_H") else v & 0xFFFFFFFF
        if name == "AF_STAT_DATA":
            return self.af[regs.f("AF_STAT_ADDR", "zone_addr")]
        if name == "AEC_CTRL":
            return regs.value["AEC_CTRL"] | (0x2 if self.aec_commit else 0)
        if name in self.ro:
            return self.ro[name]
        return regs.value[name]


# ---------------------------------------------------------------- readout script
def zone_sample(n, row):
    """Zone addresses read back: all of a small grid, a spread plus the edges of
    a large one, and two addresses beyond the grid (ALG-AEC-03 / ALG-AWB-04)."""
    zs = set(range(n)) if n <= 64 else set(range(0, n, max(1, n // 40))) | {row - 1, n - row, n - 1}
    return sorted(zs) + [n, 2 ** 20]


def readout_script(r):
    """Software reads of every statistics result, through the readout muxes."""
    st, out = r.stats, []

    def rd(name):
        out.append("%s %d\n" % (name, st.read(r, name)))

    def sel(name, v):
        r.write(name, v)
        out.append("SET %s %d\n" % (name, v))

    for n in ("AEC_CTRL", "AEC_STATUS", "AEC_FRAME_ID", "AEC_RESULT_CONTEXT_ID"):
        rd(n)
    for c in range(4):
        sel("AEC_CHANNEL_SEL", c)
        for n in ("AEC_GLOBAL_SUM_LO", "AEC_GLOBAL_SUM_HI", "AEC_GLOBAL_COUNT"):
            rd(n)
    a = st.aec
    if a is not None:
        nx, ny = max(a["nx"], 1), max(a["ny"], 1)
        for z in zone_sample(nx * ny, nx):
            sel("AEC_ZONE_ADDR", z & 0x3FF)
            rd("AEC_ZONE_GREEN_OE_UE")
            rd("AEC_ZONE_GREEN_MIN_MAX")
            for c in range(4):
                sel("AEC_CHANNEL_SEL", c)
                rd("AEC_ZONE_SUM")
                rd("AEC_ZONE_COUNT")
    for b in range(64):
        sel("AEC_HIST_ADDR", b)
        rd("AEC_HIST_DATA")
    # AWB (MAS §6.22.4.1): every READY result in completion order, each one
    # acquired, read in full and released; reads without a lease return 0.
    for n in ("AWB_STATUS", "AWB_RESULT_STATUS", "AWB_DROP_COUNT", "AWB_FRAME_ID", "AWB_GLOBAL_COUNT"):
        rd(n)
    while st.awb_ready:
        sel("AWB_RESULT_CTRL", 1)       # ACQUIRE
        rd("AWB_RESULT_STATUS")
        for n in ("AWB_FRAME_ID", "AWB_RESULT_CONTEXT_ID", "AWB_GLOBAL_COUNT") + tuple(
                "AWB_GLOBAL_SUM_%s_%s" % (c, hl) for c in "RGB" for hl in "HL"):
            rd(n)
        b = st._awb_lease()["res"]
        nx, ny = max(b["nx"], 1), max(b["ny"], 1)
        for z in zone_sample(nx * ny, nx):
            sel("AWB_ZONE_ADDR", z & 0x7FF)
            for n in ("AWB_ZONE_COUNT",) + tuple("AWB_ZONE_SUM_%s_%s" % (c, hl) for c in "RGB" for hl in "HL"):
                rd(n)
        sel("AWB_RESULT_CTRL", 2)       # RELEASE
    rd("AWB_RESULT_STATUS")
    for n in ("AF_STATUS", "AF_FRAME_ID", "AF_RESULT_CONTEXT_ID"):
        rd(n)
    for z in range(16):
        sel("AF_STAT_ADDR", z)
        rd("AF_STAT_DATA")
    return out
