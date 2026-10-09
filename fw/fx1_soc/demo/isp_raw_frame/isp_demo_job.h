/*
 * Host <-> firmware contract of the ISP RAW-frame demo
 * (platforms/fx1_soc/demo/run_isp_demo.py writes the job and the profile with
 * fx1_soc --load, and reads the result with --dump). All little-endian.
 *
 *   FX1_FW_SHARED_BASE + 0x0000  isp_demo_job      (host -> firmware)
 *   FX1_FW_SHARED_BASE + 0x0100  isp_demo_result   (firmware -> host)
 *   FX1_FW_SHARED_BASE + 0x1000  profile: profile_count x {u32 offset, u32 value},
 *                                ISP CSR writes applied in order (a .csrw file)
 *   in_addr / y_addr / uv_addr   RAW input (ISP 16-bit container) and NV12 output
 */
#ifndef ISP_DEMO_JOB_H
#define ISP_DEMO_JOB_H

#include <stdint.h>

#define ISP_DEMO_JOB_OFFSET     0x0000u
#define ISP_DEMO_RESULT_OFFSET  0x0100u
#define ISP_DEMO_PROFILE_OFFSET 0x1000u
#define ISP_DEMO_JOB_MAGIC      0x4A315846u /* "FX1J" */
#define ISP_DEMO_RESULT_MAGIC   0x52315846u /* "FX1R" */
#define ISP_DEMO_VERSION        1u

typedef struct isp_demo_job {
    uint32_t magic, version;
    uint32_t width, height, bayer; /* bayer: COMMON_BAYER code (RGGB 0, GRBG 1, GBRG 2, BGGR 3) */
    uint32_t in_addr, in_stride;   /* RAW input, 16-bit samples, stride in bytes */
    uint32_t y_addr, uv_addr, out_stride;
    uint32_t profile_count;        /* {offset, value} pairs at ISP_DEMO_PROFILE_OFFSET */
    uint32_t frames;               /* the same input processed `frames` times (temporal blocks) */
    uint32_t reserved[4];
} isp_demo_job;

typedef struct isp_demo_result {
    uint32_t magic, status; /* status 0 = frames completed */
    uint32_t out_width, out_height, out_stride;
    uint32_t frames_done, idma_frames, odma_frames, dma_err;
    uint32_t aec_frame_id, awb_frame_id, af_frame_id;
    uint32_t frame_ticks;   /* mtime ticks (50 ns) from the first queue to the last completion */
    uint32_t reserved[3];
} isp_demo_result;

#endif /* ISP_DEMO_JOB_H */
