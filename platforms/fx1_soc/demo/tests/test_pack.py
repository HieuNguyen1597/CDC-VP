#!/usr/bin/env python3
"""run_isp_demo.py --pack-only on small synthetic dumps (review G6a R2).

One 4x4 job packs to a 24-byte NV12 and a PNG; every damaged result block,
dump, job or preview must make the script exit 2 and leave no platform.nv12
or platform.png behind. No simulator is needed: the dumps are written here
the way fx1_soc --dump writes them. Needs python3 with numpy and Pillow (the
preview tool).
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
SCRIPT = os.path.join(HERE, "..", "run_isp_demo.py")
JOB_MAGIC, RESULT_MAGIC = 0x4A315846, 0x52315846
W = H = 4
STRIDE = 16                       # align(W, 16), what the script computes
Y_LEN, UV_LEN = STRIDE * H, STRIDE * (H // 2)


def job(**kw):
    f = dict(width=W, height=H, in_stride=16, out_stride=STRIDE, profile_count=1, frames=1)
    f.update(kw)
    return struct.pack("<16I", JOB_MAGIC, 1, f["width"], f["height"], 3, 0x81000000, f["in_stride"],
                       0x88000000, 0x88100000, f["out_stride"], f["profile_count"], f["frames"], 0, 0, 0, 0)


def result(**kw):
    f = dict(magic=RESULT_MAGIC, status=0, out_width=W, out_height=H, out_stride=STRIDE, frames_done=1,
             idma_frames=1, odma_frames=1, dma_err=2, aec=1, awb=1, af=1, ticks=0)
    f.update(kw)
    return struct.pack("<13I", *f.values())


def case(name, expect, files=None, drop=(), png_dir=False, profile=None):
    d = tempfile.mkdtemp(prefix="fx1_pack_")
    try:
        content = {"job.bin": job(), "result.bin": result(),
                   "y.bin": bytes(range(Y_LEN)), "uv.bin": bytes(128 + i % 64 for i in range(UV_LEN))}
        content.update(files or {})
        for n, data in content.items():
            if n not in drop:
                open(os.path.join(d, n), "wb").write(data)
        with open(os.path.join(d, "p.csrw"), "w") as f:
            f.write("DMA_CTRL 0x0\n")
        if png_dir:
            os.mkdir(os.path.join(d, "platform.png"))   # the preview cannot write its output
        else:
            open(os.path.join(d, "platform.png"), "wb").write(b"stale")
        r = subprocess.run([sys.executable, SCRIPT, "--pack-only", "--no-reference", "--out", d,
                            "--profile", profile or os.path.join(d, "p.csrw")],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        nv12, png = os.path.join(d, "platform.nv12"), os.path.join(d, "platform.png")
        if expect == 0:
            ok = (r.returncode == 0 and os.path.getsize(nv12) == W * H * 3 // 2 and os.path.isfile(png)
                  and open(png, "rb").read(8) == b"\x89PNG\r\n\x1a\n")
        else:
            ok = (r.returncode == expect and not os.path.exists(nv12)
                  and (png_dir or not os.path.exists(png)))
        last = (r.stdout.strip().splitlines() or [""])[-1]
        print("%-4s %-44s exit %d  %s" % ("ok" if ok else "FAIL", name, r.returncode, last))
        return ok
    finally:
        shutil.rmtree(d)


def main():
    default_profile = os.path.join(HERE, "..", "..", "..", "..", "components", "FX1_Components", "isp", "samples",
                                   "profiles", "outdoor1_bggr_2688x1520_full.csrw")
    results = [
        case("valid 4x4 job", 0),
        case("Y and UV dumps of 1 byte (review repro)", 2, {"y.bin": b"\0", "uv.bin": b"\0"}),
        case("Y dump one byte short", 2, {"y.bin": bytes(Y_LEN - 1)}),
        case("UV dump one byte long", 2, {"uv.bin": bytes(UV_LEN + 1)}),
        case("result block truncated", 2, {"result.bin": result()[:48]}),
        case("result magic wrong", 2, {"result.bin": result(magic=0)}),
        case("result status failed", 2, {"result.bin": result(status=1)}),
        case("stride below width", 2, {"result.bin": result(out_stride=2)}),
        case("odd output height", 2, {"result.bin": result(out_height=3)}),
        case("zero output width", 2, {"result.bin": result(out_width=0)}),
        case("output larger than input", 2, {"result.bin": result(out_width=8)}),
        case("no frame completed", 2, {"result.bin": result(frames_done=0, odma_frames=0)}),
        case("job.bin missing", 2, drop=("job.bin",)),
        case("job.bin of another version", 2, {"job.bin": job()[:4] + struct.pack("<I", 9) + job()[8:]}),
        case("--profile differs from the job's", 2, profile=default_profile),
        case("preview cannot write the PNG", 2, png_dir=True),
    ]
    print("%d/%d pack cases pass" % (sum(results), len(results)))
    return 0 if all(results) else 1


if __name__ == "__main__":
    sys.exit(main())
