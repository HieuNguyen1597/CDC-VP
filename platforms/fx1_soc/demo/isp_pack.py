#!/usr/bin/env python3
"""Pack the ISP demo's DDR dumps into NV12 and a PNG preview; Python stdlib only.

The demo firmware (bare-metal isp_demo or `isp run` in the FreeRTOS CLI)
leaves a result block and the NV12 planes in DDR; fx1_soc --dump writes them
to result.bin, y.bin and uv.bin next to the job.bin they were made from. This
module checks them, keeps the active bytes (platform.nv12: Y rows, then UV
rows) and writes platform.png.

Used by run_isp_demo.py (CDC-VP) and shipped as is in the SDK, where no numpy
or Pillow can be assumed. As a command:

  isp_pack.py DIR [--expect-sha256 HEX]

Exit status: 0 packed (and equal to HEX when given), 1 NV12 differs from HEX,
2 the inputs were rejected.
"""
import argparse
import hashlib
import os
import struct
import sys
import zlib

# fw/fx1_soc/demo/isp_raw_frame/isp_demo_job.h
JOB_MAGIC, RESULT_MAGIC, VERSION = 0x4A315846, 0x52315846, 1
JOB_SIZE = 64
RESULT_FIELDS = ("magic status out_width out_height out_stride frames_done idma_frames odma_frames "
                 "dma_err aec_frame_id awb_frame_id af_frame_id frame_ticks").split()
RESULT_SIZE = 4 * len(RESULT_FIELDS)
BAYER = {"RGGB": 0, "GRBG": 1, "GBRG": 2, "BGGR": 3}  # COMMON_BAYER codes (DEC-11)
KR_KB = {0: (0.299, 0.114), 1: (0.2126, 0.0722)}       # BT.601, BT.709 (CSC_CTRL.std)


class PackError(Exception):
    """The inputs cannot hold a valid frame; the message says why."""


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def read_job(path):
    """job.bin (isp_demo_job.h): the geometry the dumps were made with."""
    data = open(path, "rb").read()
    if len(data) != JOB_SIZE:
        raise PackError("%s: %d bytes, a job descriptor has %d" % (path, len(data), JOB_SIZE))
    f = struct.unpack("<16I", data)
    if f[0] != JOB_MAGIC or f[1] != VERSION or f[4] not in BAYER.values():
        raise PackError("%s: not a version-%d job descriptor" % (path, VERSION))
    return dict(width=f[2], height=f[3], bayer=f[4], in_stride=f[6], y_addr=f[7], uv_addr=f[8], out_stride=f[9],
                profile_count=f[10], frames=f[11])


def read_result(path):
    """result.bin: the block the firmware writes once the frames are done."""
    data = open(path, "rb").read()
    if len(data) != RESULT_SIZE:
        raise PackError("result.bin: %d bytes, the result block has %d" % (len(data), RESULT_SIZE))
    res = dict(zip(RESULT_FIELDS, struct.unpack("<%dI" % len(RESULT_FIELDS), data)))
    if res["magic"] != RESULT_MAGIC or res["status"] != 0:
        raise PackError("no valid result block from the firmware (was `isp run` PASS before poweroff?)")
    return res


def check_output(res, width, height, frames, out_stride, y, uv):
    """Reject a result or dump that cannot hold the frame (review G6a R2): slicing
    short data would otherwise give a truncated NV12 without any error."""
    ow, oh, stride = res["out_width"], res["out_height"], res["out_stride"]
    y_len, uv_len = out_stride * height, out_stride * (height // 2)
    problems = []
    if not (0 < ow <= width and 0 < oh <= height) or ow % 2 or oh % 2:
        problems.append("output %dx%d is not an even size within the %dx%d input" % (ow, oh, width, height))
    if stride != out_stride or stride < ow:
        problems.append("output stride %d, the job used %d (width %d)" % (stride, out_stride, ow))
    if res["frames_done"] != frames or res["odma_frames"] != frames or res["idma_frames"] != frames:
        problems.append("frames done %d, IDMA %d, ODMA %d, the job asked for %d"
                        % (res["frames_done"], res["idma_frames"], res["odma_frames"], frames))
    if len(y) != y_len or len(uv) != uv_len:
        problems.append("dumps hold Y %d and UV %d bytes, expected %d and %d" % (len(y), len(uv), y_len, uv_len))
    if problems:
        raise PackError("output rejected: " + "; ".join(problems))


def pack_nv12(y, uv, width, height, stride):
    """The active bytes: Y rows, then the interleaved UV rows."""
    rows = [y[r * stride:r * stride + width] for r in range(height)]
    rows += [uv[r * stride:r * stride + width] for r in range(height // 2)]
    return b"".join(rows)


def _clip_table(values):
    return bytes(0 if v < 0 else 255 if v > 255 else v for v in values)


def nv12_to_png(nv12, width, height, path, std=0):
    """Write an RGB PNG. Same inverse transform as isp/tools/nv12_preview.py:
    limited-range YCbCr (Y 16..235, C 16..240), BT.601 or BT.709, chroma by
    nearest neighbour. A preview is a visual check, not a pass/fail criterion."""
    if len(nv12) != width * height * 3 // 2:
        raise PackError("NV12 holds %d bytes, %dx%d needs %d" % (len(nv12), width, height, width * height * 3 // 2))
    kr, kb = KR_KB[std]
    kg = 1 - kr - kb
    # R = Y' + a*Pr, B = Y' + b*Pb, G = Y' - c*Pr - d*Pb, all in 255 units; one
    # rounding at the end, offset by 1024 so the tables index non-negative sums.
    ylut = [(v - 16) / 219.0 * 255 for v in range(256)]
    r_v = [2 * (1 - kr) * (v - 128) / 224.0 * 255 for v in range(256)]
    b_u = [2 * (1 - kb) * (u - 128) / 224.0 * 255 for u in range(256)]
    g_v = [-kr * 2 * (1 - kr) * (v - 128) / 224.0 * 255 / kg for v in range(256)]
    g_u = [-kb * 2 * (1 - kb) * (u - 128) / 224.0 * 255 / kg for u in range(256)]
    clip = _clip_table(range(-1024, 1280))
    raw = bytearray()
    ysize = width * height
    for row in range(height):
        yrow = nv12[row * width:(row + 1) * width]
        uvrow = nv12[ysize + (row // 2) * width:ysize + (row // 2 + 1) * width]
        out = bytearray(3 * width + 1)  # filter byte 0, then RGB
        for x in range(0, width, 2):
            u, v = uvrow[x], uvrow[x + 1]
            rv, bu, gc = r_v[v], b_u[u], g_v[v] + g_u[u]
            for k in (x, x + 1):
                yy = ylut[yrow[k]]
                o = 1 + 3 * k
                out[o] = clip[int(yy + rv + 1024.5)]
                out[o + 1] = clip[int(yy + gc + 1024.5)]
                out[o + 2] = clip[int(yy + bu + 1024.5)]
        raw += out

    def chunk(tag, data):
        return struct.pack(">I", len(data)) + tag + data + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n")
        f.write(chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0)))
        f.write(chunk(b"IDAT", zlib.compress(bytes(raw), 6)))
        f.write(chunk(b"IEND", b""))


def pack_dir(d, expect_sha=None):
    """job.bin + result.bin + y.bin + uv.bin in d -> platform.nv12, platform.png, summary.txt.
    Returns (result, sha); raises PackError. Nothing named platform.* survives a failure."""
    out = lambda name: os.path.join(d, name)
    for name in ("platform.nv12", "platform.png", "summary.txt"):
        if os.path.isfile(out(name)):
            os.remove(out(name))
    for name in ("job.bin", "result.bin", "y.bin", "uv.bin"):
        if not os.path.isfile(out(name)):
            raise PackError("%s not found (the VP writes the dumps at `poweroff`)" % out(name))
    job = read_job(out("job.bin"))
    res = read_result(out("result.bin"))
    y, uv = open(out("y.bin"), "rb").read(), open(out("uv.bin"), "rb").read()
    check_output(res, job["width"], job["height"], job["frames"], job["out_stride"], y, uv)
    ow, oh = res["out_width"], res["out_height"]
    nv12 = pack_nv12(y, uv, ow, oh, res["out_stride"])
    with open(out("platform.nv12"), "wb") as f:
        f.write(nv12)
    try:
        nv12_to_png(nv12, ow, oh, out("platform.png"))
    except OSError as e:
        os.remove(out("platform.nv12"))
        raise PackError("cannot write %s: %s" % (out("platform.png"), e))
    sha = hashlib.sha256(nv12).hexdigest()
    verdict = "-" if not expect_sha else ("MATCH" if sha == expect_sha.lower() else "MISMATCH")
    lines = [
        "input          %dx%d %s, %d frame(s)" % (job["width"], job["height"],
                                                   next(k for k, v in BAYER.items() if v == job["bayer"]), job["frames"]),
        "output         %dx%d NV12, stride %d" % (ow, oh, res["out_stride"]),
        "ISP counters   IDMA %d, ODMA %d frames; DMA_ERR 0x%x" % (res["idma_frames"], res["odma_frames"], res["dma_err"]),
        "FRAME_ID       AEC %d, AWB %d, AF %d" % (res["aec_frame_id"], res["awb_frame_id"], res["af_frame_id"]),
        "platform NV12  %s  sha256 %s" % (out("platform.nv12"), sha),
        "expected       %s" % (expect_sha or "-"),
        "result         %s" % verdict,
        "preview        %s" % out("platform.png"),
    ]
    with open(out("summary.txt"), "w") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return res, sha, verdict


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("dir", help="directory with job.bin, result.bin, y.bin and uv.bin")
    ap.add_argument("--expect-sha256", help="NV12 SHA-256 the run must reproduce")
    a = ap.parse_args()
    try:
        _, _, verdict = pack_dir(a.dir, a.expect_sha256)
    except PackError as e:
        print("isp_pack: " + str(e), file=sys.stderr)
        return 2
    return 1 if verdict == "MISMATCH" else 0


if __name__ == "__main__":
    sys.exit(main())
