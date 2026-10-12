/* SPDX-License-Identifier: Apache-2.0 */
/* FX1 ISP reference driver; see fx1_isp_drv.h and docs/ISP_PROGRAMMING_GUIDE.md. */

#include "fx1_isp/fx1_isp_drv.h"

#include "fx1_isp/fx1_isp_csr.h"

#define RD(dev, off) ((dev)->hal.read32((dev)->hal.ctx, (off)))
#define WR(dev, off, v) ((dev)->hal.write32((dev)->hal.ctx, (off), (v)))

#define MAX_WIDTH 3840u      /* PARA_MAX_H_ACTIVE */
#define MAX_HEIGHT 2160u     /* PARA_MAX_V_ACTIVE */
#define LSC_MESH_MAX 128u    /* nodes per axis (MAS §6.8, CSR 07/10; DEC-31 superseded) */
#define SOFT_RESET_CYCLES 15u /* soft-reset pulse, core clocks (MAS §6.5.3) */
/* The Resizer derives its configuration from a stable key in 35 core clocks
 * (MAS §6.20.4.3, Table 123) and only then publishes RESIZER_OUT_W/H; a frame
 * started earlier uses the previous set. A key changed while a derivation runs
 * is derived after it, so publication can take two derivations after the last
 * change: 2 x 36 edges, plus margin (review U4-R1). RESIZER_OUT_W/H alone
 * cannot tell a finished derivation (a new key may give the same size), so the
 * driver waits this long before reading them or starting a frame. */
#define RESIZER_SETTLE_CYCLES 80u
#define STATS_RETRIES 4
#define DRV_MAGIC 0x46583149u /* "FX1I": set by fx1_isp_init */

static int initialised(const fx1_isp_dev *dev) { return dev && dev->magic == DRV_MAGIC; }
static int streaming(const fx1_isp_dev *dev) { return initialised(dev) && dev->state == FX1_ISP_STATE_STREAMING; }

/* Zone/histogram memory is in place (DEC-30): after an abort or a reset it
 * holds partial or cleared data under the previous FRAME_ID (M3-A7). Readers
 * refuse it until a new AEC publication moves FRAME_ID. */
/* Every event moves the mark to the current FRAME_ID: an older mark could
 * already be passed by a publication nobody read. No publication can happen
 * between an IDMA abort and its re-arm (the IDMA stops on the failed input). */
static void stats_untrust(fx1_isp_dev *dev, int aborted) {
    /* FRAME_ID 0 can also mean the first frame was aborted after writing
     * partial zone/histogram data. It is not a valid publication. */
    dev->stats_mark = RD(dev, FX1_ISP_AEC_FRAME_ID_OFFSET);
    dev->stats_untrusted = aborted || dev->stats_mark != 0u;
}

/* Removes queue entry k (0 = oldest). */
static fx1_isp_frame queue_remove(fx1_isp_dev *dev, unsigned k) {
    unsigned i;
    const fx1_isp_frame f = dev->queue[k];
    for (i = k; i + 1u < dev->in_flight; ++i) {
        dev->queue[i] = dev->queue[i + 1u];
    }
    --dev->in_flight;
    return f;
}

static void delay(fx1_isp_dev *dev, uint32_t cycles) {
    if (dev->hal.delay_cycles) {
        dev->hal.delay_cycles(dev->hal.ctx, cycles);
    }
}

/* The statistics result memories have synchronous read ports: a word is
 * valid one core clock after its address is selected (MAS §6.21.5.1 p237,
 * §6.22.4.3), and an AWB lease is readable one clock after ACQUIRE. A CSR
 * transaction is not guaranteed to last a core clock (csr_latency and the
 * core clock are independent), so the driver waits explicitly. */
#define RAM_READ_CYCLES 1u

static void select_word(fx1_isp_dev *dev, uint32_t addr_off, uint32_t addr) {
    WR(dev, addr_off, addr);
    delay(dev, RAM_READ_CYCLES);
}

static void write_addr(fx1_isp_dev *dev, uint32_t lo_off, uint64_t addr) {
    WR(dev, lo_off, (uint32_t)addr);
    WR(dev, lo_off + 4u, (uint32_t)(addr >> 32));
}

/* Rotation positions restart at 0 after i_rst_n and soft reset (M2-A1). The
 * frame counters are kept, so the driver remembers where it started. */
static void restart_rotation(fx1_isp_dev *dev) {
    dev->in_next = dev->out_next = dev->out_done = 0;
    dev->in_flight = 0;
    dev->in_failed = 0;
    dev->lost_count = 0;
}

/* Queue position of the frame the IDMA failed on: in queue order, the inputs
 * the IDMA retired and the one it released have VALID clear, later ones are
 * still VALID. Returns -1 if none. */
static int idma_failed_pos(const fx1_isp_dev *dev, uint32_t valid) {
    int found = -1;
    unsigned k;
    for (k = 0; k < dev->in_flight; ++k) {
        if (!(valid & (1u << dev->queue[k].input))) {
            found = (int)k;
        }
    }
    return found;
}

/* A frame whose output failed is lost once the IDMA has retired its input
 * without an error on it; it is then removed and the outputs of the later
 * frames move up one buffer, as the ODMA kept its rotation position (DEC-19).
 * VALID is read before DMA_ERR: an IDMA error releases VALID and flags
 * IDMA_AXI together, so an error on the frame cannot be missed. */
static unsigned settle_losses(fx1_isp_dev *dev) {
    unsigned k = 0, n = 0;
    while (k < dev->in_flight) {
        const fx1_isp_frame f = dev->queue[k];
        if (!f.odma_failed) {
            ++k;
            continue;
        }
        const uint32_t valid = RD(dev, FX1_ISP_IDMA_BUF_VALID_OFFSET);
        if (valid & (1u << f.input)) {
            ++k;  /* the IDMA is still reading it */
            continue;
        }
        if ((RD(dev, FX1_ISP_DMA_ERR_OFFSET) & FX1_ISP_DMA_ERR_ERR_AXI_RD_BIT) && idma_failed_pos(dev, valid) == (int)k) {
            ++k;  /* the IDMA failed on this frame: recover retries it */
            continue;
        }
        queue_remove(dev, k);
        dev->out_next = (dev->out_next + FX1_ISP_NUM_BUFFERS - 1u) % FX1_ISP_NUM_BUFFERS;
        if (dev->lost_count < 4u) {
            dev->lost[dev->lost_count++] = f;
        }
        ++n;
    }
    return n;
}

int fx1_isp_init(fx1_isp_dev *dev, const fx1_isp_hal *hal) {
    if (!dev || !hal || !hal->read32 || !hal->write32 || !hal->delay_cycles) {
        return FX1_ISP_EINVAL;
    }
    dev->hal = *hal;
    dev->magic = DRV_MAGIC;
    dev->state = FX1_ISP_STATE_CONFIG;
    dev->out_width = dev->out_height = 0;
    dev->dma_ctrl = 0;
    dev->started = 0;
    dev->next_seq = 0;
    dev->gtm_en = 0;
    fx1_isp_soft_reset(dev);
    return FX1_ISP_OK;
}

int fx1_isp_stop(fx1_isp_dev *dev) {
    if (!streaming(dev)) {
        return FX1_ISP_ESTATE;
    }
    const int dropped = (int)dev->in_flight;
    /* Disable before resetting: if DMA_EN survives into the 15-cycle window,
     * IDMA sees no VALID buffer afterwards and reasserts UNDERRUN. */
    WR(dev, FX1_ISP_DMA_CTRL_OFFSET, dev->dma_ctrl & ~(FX1_ISP_DMA_CTRL_IDMA_EN_MASK | FX1_ISP_DMA_CTRL_ODMA_EN_MASK));
    fx1_isp_soft_reset(dev);  /* frames in flight are discarded (DEC-14) */
    WR(dev, FX1_ISP_COMMON_CTRL_OFFSET, 0);  /* CONFIG state: pipeline disabled */
    dev->started = 0;
    dev->state = FX1_ISP_STATE_CONFIG;
    return dropped;
}

void fx1_isp_soft_reset(fx1_isp_dev *dev) {
    if (!initialised(dev)) {
        return;
    }
    /* COMMON_CTRL.soft_rst is W1SC; isp_en is RW and written back unchanged. */
    const uint32_t ctrl = RD(dev, FX1_ISP_COMMON_CTRL_OFFSET) & FX1_ISP_COMMON_CTRL_ISP_EN_MASK;
    WR(dev, FX1_ISP_COMMON_CTRL_OFFSET, ctrl | FX1_ISP_COMMON_CTRL_SOFT_RST_MASK);
    delay(dev, SOFT_RESET_CYCLES + 8u); /* W1S/W1C writes are ignored during the window */
    /* The Resizer published set returned to its reset value and is derived
     * again after the pulse: no frame before it is published (MAS Table 124). */
    delay(dev, RESIZER_SETTLE_CYCLES);
    WR(dev, FX1_ISP_COMMON_IRQ_STATUS_OFFSET, 0xFFFFFFFFu);
    WR(dev, FX1_ISP_DMA_IRQ_STAT_OFFSET, 0xFFFFFFFFu);
    WR(dev, FX1_ISP_DMA_ERR_OFFSET, 0xFFFFFFFFu);
    restart_rotation(dev);
    stats_untrust(dev, 0);
    dev->awb_leased = 0;     /* the COMMON soft reset clears AWB ownership (MAS p248) */
    dev->awb_last_valid = 0;
    /* The soft reset keeps the EE configuration: the selector as programmed. */
    dev->ee_sel = (RD(dev, FX1_ISP_EE_CTRL_OFFSET) & FX1_ISP_EE_CTRL_PROFILE_SEL_MASK) >> FX1_ISP_EE_CTRL_PROFILE_SEL_SHIFT;
    if (dev->ee_sel > 2u) {
        dev->ee_sel = 0;  /* code 3 selects profile 0 */
    }
    dev->ee_in_use = 1u << dev->ee_sel;
    dev->gtm_manual = dev->gtm_wait = 0;
}

int fx1_isp_dma_reset(fx1_isp_dev *dev) {
    if (!streaming(dev)) {
        return FX1_ISP_ESTATE;
    }
    const int dropped = (int)dev->in_flight;
    /* A frame in flight whose own output is not DONE may have had its SOF
     * and overwritten zone/histogram data; a frame whose output is DONE
     * published normally. Frame k of the queue owns output out_done + k (the
     * rotation the other DONE checks use); a DONE bit of any other buffer
     * says nothing about the frames in flight (review U6-R3). After an ODMA
     * failure the later frames sit one buffer earlier than this mapping
     * until the loss is settled, so the last frame in flight maps to a
     * buffer nothing has written: the check stays conservative. So does a
     * completion between this read and the reset. */
    const uint32_t done = RD(dev, FX1_ISP_ODMA_BUF_DONE_OFFSET);
    int unfinished = 0;
    unsigned k;
    for (k = 0; k < dev->in_flight; ++k) {
        if (!(done & (1u << ((dev->out_done + k) % FX1_ISP_NUM_BUFFERS)))) {
            unfinished = 1;
            break;
        }
    }
    /* DMA_CTRL.soft_reset is W1SC; the enables and burst length are RW and
     * written back unchanged (DEC-14). The frame in flight is discarded and the
     * pipeline flushed to a frame boundary; configuration, statistics results
     * and AWB ownership are kept (MAS Table 183). */
    /* The driver wrote DMA_CTRL itself at the first queue; reading it back
     * would only delay the reset of the frame in flight. */
    const uint32_t ctrl = dev->started ? dev->dma_ctrl : RD(dev, FX1_ISP_DMA_CTRL_OFFSET);
    WR(dev, FX1_ISP_DMA_CTRL_OFFSET, ctrl | FX1_ISP_DMA_CTRL_SOFT_RESET_MASK);
    delay(dev, SOFT_RESET_CYCLES + 8u);
    WR(dev, FX1_ISP_DMA_IRQ_STAT_OFFSET, 0xFFFFFFFFu);
    WR(dev, FX1_ISP_DMA_ERR_OFFSET, 0xFFFFFFFFu);
    restart_rotation(dev);
    /* An aborted frame's partial accumulation overwrote the AEC zone and
     * histogram results (MAS §9.1.1). Without one, the published data stays
     * as trusted as it was (review U6-R1). */
    if (unfinished) {
        stats_untrust(dev, 1);
    }
    /* GTM hand-over (MAS §9.2 step 2): the frames counted at step 1 that the
     * reset dropped will never be taken. A dropped frame builds no curve
     * (MAS §9.1.1), so the next frame taken after the reset, queued after
     * step 1, is the one whose SOF consumes any pending curve. Cold as at
     * step 1: no frame taken since i_rst_n and nothing queued (review U6-R2). */
    if (dev->gtm_wait) {
        const uint32_t count = RD(dev, FX1_ISP_IDMA_FRAME_COUNT_OFFSET);
        if ((int32_t)(count - dev->gtm_target) < 0) {
            dev->gtm_target = count + 1u;
            dev->gtm_wait = count != 0u;
        }
    }
    return dropped;
}

int fx1_isp_set_geometry(fx1_isp_dev *dev, uint32_t width, uint32_t height, uint32_t bayer) {
    if (!initialised(dev)) {
        return FX1_ISP_ESTATE;
    }
    if (width < 2u || width > MAX_WIDTH || (width & 1u) || height < 2u || height > MAX_HEIGHT || (height & 1u) ||
        bayer > 3u) {
        return FX1_ISP_EINVAL;
    }
    if ((RD(dev, FX1_ISP_COMMON_STATUS_OFFSET) & FX1_ISP_COMMON_STATUS_BUSY_MASK) || dev->in_flight) {
        return FX1_ISP_EBUSY; /* HAS §9.4.4: geometry changes with the pipeline idle */
    }
    WR(dev, FX1_ISP_COMMON_FRAME_WIDTH_OFFSET, width);
    WR(dev, FX1_ISP_COMMON_FRAME_HEIGHT_OFFSET, height);
    WR(dev, FX1_ISP_COMMON_BAYER_OFFSET, bayer);
    return FX1_ISP_OK;
}

void fx1_isp_apply(fx1_isp_dev *dev, const fx1_isp_reg_write *writes, size_t n) {
    size_t i;
    if (!initialised(dev) || !writes) {
        return;
    }
    for (i = 0; i < n; ++i) {
        WR(dev, writes[i].offset, writes[i].value);
    }
}

void fx1_isp_load_gamma(fx1_isp_dev *dev, const uint16_t lut[256]) {
    unsigned i;
    if (!initialised(dev) || !lut) {
        return;
    }
    WR(dev, FX1_ISP_GAMMA_LUT_ADDR_OFFSET, 0);  /* the address auto-increments per DATA write */
    for (i = 0; i < 256u; ++i) {
        WR(dev, FX1_ISP_GAMMA_LUT_DATA_OFFSET, lut[i]);
    }
}

int fx1_isp_load_ee_table(fx1_isp_dev *dev, unsigned bank, const uint16_t *table, unsigned n) {
    unsigned i;
    if (!initialised(dev)) {
        return FX1_ISP_ESTATE;
    }
    if (bank > 3u || n != (bank < 2u ? 64u : 32u) || !table) {
        return FX1_ISP_EINVAL;
    }
    const uint32_t ctrl = RD(dev, FX1_ISP_EE_LUT_CTRL_OFFSET) & ~FX1_ISP_EE_LUT_CTRL_LUT_SEL_MASK;
    WR(dev, FX1_ISP_EE_LUT_CTRL_OFFSET, ctrl | (bank << FX1_ISP_EE_LUT_CTRL_LUT_SEL_SHIFT));
    WR(dev, FX1_ISP_EE_LUT_ADDR_OFFSET, 0);
    for (i = 0; i < n; ++i) {
        WR(dev, FX1_ISP_EE_LUT_WDATA_OFFSET, table[i]);
    }
    return FX1_ISP_OK;
}

int fx1_isp_gtm_manual_begin(fx1_isp_dev *dev) {
    if (!initialised(dev)) {
        return FX1_ISP_ESTATE;
    }
    const uint32_t ctrl = RD(dev, FX1_ISP_GTM_CTRL_OFFSET);
    /* IDMA_FRAME_COUNT counts the frames the pipeline took completely: every
     * frame whose end built a curve, also when its output then failed (review
     * U3-R3). Only i_rst_n clears it; soft reset and fx1_isp_init keep it. */
    const uint32_t count = RD(dev, FX1_ISP_IDMA_FRAME_COUNT_OFFSET);
    /* Step 1: EN = 0 (table writes accepted, frames pass through), MANUAL = 1
     * (no new automatic curve; a build already running still completes). */
    if (!dev->gtm_manual) {
        dev->gtm_en = (ctrl & FX1_ISP_GTM_CTRL_EN_MASK) != 0;
    }
    WR(dev, FX1_ISP_GTM_CTRL_OFFSET, (ctrl & ~FX1_ISP_GTM_CTRL_EN_MASK) | FX1_ISP_GTM_CTRL_MANUAL_MASK);
    dev->gtm_manual = 1;
    /* Step 2: a curve built at the end of an earlier frame waits for the next
     * SOF. Every frame in flight may have started before step 1, so the frame
     * queued after them must be taken by the pipeline: its SOF consumed any
     * pending curve, and MANUAL kept its end from building one. Cold only when
     * no frame has been taken since i_rst_n and none is queued. */
    dev->gtm_wait = !(count == 0u && dev->in_flight == 0);
    dev->gtm_target = count + dev->in_flight + 1u;
    return FX1_ISP_OK;
}

int fx1_isp_load_gtm_lut(fx1_isp_dev *dev, const uint16_t lut[65]) {
    unsigned i;
    if (!initialised(dev)) {
        return FX1_ISP_ESTATE;
    }
    if (!lut) {
        return FX1_ISP_EINVAL;
    }
    if (!dev->gtm_manual) {
        fx1_isp_gtm_manual_begin(dev);
    }
    if (dev->gtm_wait && (int32_t)(RD(dev, FX1_ISP_IDMA_FRAME_COUNT_OFFSET) - dev->gtm_target) < 0) {
        return FX1_ISP_EAGAIN;  /* step 2 not done: a pending automatic curve could replace the load */
    }
    WR(dev, FX1_ISP_GTM_LUT_ADDR_OFFSET, 0);
    for (i = 0; i < 65u; ++i) {
        WR(dev, FX1_ISP_GTM_LUT_DATA_OFFSET, lut[i]);
    }
    for (i = 0; i < 65u; ++i) {  /* step 4 */
        WR(dev, FX1_ISP_GTM_LUT_ADDR_OFFSET, i);
        if ((RD(dev, FX1_ISP_GTM_LUT_RDATA_OFFSET) & 0xFFFFu) != lut[i]) {
            return FX1_ISP_EIO;
        }
    }
    /* Step 5: restore EN; MANUAL stays set so the curve persists. */
    if (dev->gtm_en) {
        const uint32_t ctrl = RD(dev, FX1_ISP_GTM_CTRL_OFFSET);
        WR(dev, FX1_ISP_GTM_CTRL_OFFSET, ctrl | FX1_ISP_GTM_CTRL_EN_MASK | FX1_ISP_GTM_CTRL_MANUAL_MASK);
    }
    dev->gtm_manual = 0;
    dev->gtm_wait = 0;
    return FX1_ISP_OK;
}

/* ---- EE resident profiles (MAS §6.18.4.1, §9.2) ---- */

static void ee_ctrl_fields(fx1_isp_dev *dev, uint32_t mask, uint32_t value) {
    const uint32_t ctrl = RD(dev, FX1_ISP_EE_CTRL_OFFSET);
    WR(dev, FX1_ISP_EE_CTRL_OFFSET, (ctrl & ~mask) | (value & mask));
}

int fx1_isp_ee_load_profile(fx1_isp_dev *dev, unsigned profile, const fx1_isp_ee_profile *p) {
    const uint32_t scalars[10] = {p ? p->alpha : 0u, p ? p->beta : 0u, p ? p->clamp : 0u, p ? p->feature_en : 0u,
                                  p ? p->radial_center : 0u, p ? p->r2_th[0] : 0u, p ? p->r2_th[1] : 0u,
                                  p ? p->r2_th[2] : 0u, p ? p->radial_gain_01 : 0u, p ? p->radial_gain_23 : 0u};
    const uint16_t *tables[4];
    unsigned k, bank, i;
    if (!initialised(dev)) {
        return FX1_ISP_ESTATE;
    }
    if (profile > 2u || !p) {
        return FX1_ISP_EINVAL;
    }
    /* A profile a frame may still use is not rewritten while EE works: any
     * profile selected since EE was last seen idle. Seen idle, no frame is in
     * EE, and the next one will use the current selection. */
    if (RD(dev, FX1_ISP_EE_STATUS_OFFSET) & FX1_ISP_EE_STATUS_BUSY_MASK) {
        if (dev->ee_in_use & (1u << profile)) {
            return FX1_ISP_EBUSY;
        }
    } else {
        dev->ee_in_use = 1u << dev->ee_sel;
    }
    tables[0] = p->luma;
    tables[1] = p->activity;
    tables[2] = p->contrast_pos;
    tables[3] = p->contrast_neg;
    ee_ctrl_fields(dev, FX1_ISP_EE_CTRL_PROFILE_WR_SEL_MASK, profile << FX1_ISP_EE_CTRL_PROFILE_WR_SEL_SHIFT);
    /* Full 32-bit writes: a partial write would merge the bytes last written
     * to the register, possibly another profile's (MAS §9.2). */
    for (k = 0; k < 10u; ++k) {
        WR(dev, FX1_ISP_EE_ALPHA_OFFSET + 4u * k, scalars[k]);
    }
    for (bank = 0; bank < 4u; ++bank) {
        const unsigned n = bank < 2u ? 64u : 32u;
        const int rc = fx1_isp_load_ee_table(dev, bank, tables[bank], n);
        if (rc != FX1_ISP_OK) {
            return rc;
        }
        WR(dev, FX1_ISP_EE_LUT_ADDR_OFFSET, 0);  /* WDATA advanced the address */
        for (i = 0; i < n; ++i) {
            WR(dev, FX1_ISP_EE_LUT_ADDR_OFFSET, i);
            if ((RD(dev, FX1_ISP_EE_LUT_RDATA_OFFSET) & 0xFFFFu) != tables[bank][i]) {
                return FX1_ISP_EIO;
            }
        }
    }
    return FX1_ISP_OK;
}

int fx1_isp_ee_select(fx1_isp_dev *dev, unsigned profile, int enable) {
    if (!initialised(dev)) {
        return FX1_ISP_ESTATE;
    }
    if (profile > 2u) {
        return FX1_ISP_EINVAL;
    }
    dev->ee_sel = profile;
    dev->ee_in_use |= 1u << profile;  /* the profiles selected before stay in use until EE is seen idle */
    ee_ctrl_fields(dev, FX1_ISP_EE_CTRL_PROFILE_SEL_MASK | FX1_ISP_EE_CTRL_EN_MASK,
                   (profile << FX1_ISP_EE_CTRL_PROFILE_SEL_SHIFT) | (enable ? FX1_ISP_EE_CTRL_EN_MASK : 0u));
    return FX1_ISP_OK;
}

int fx1_isp_lsc_load(fx1_isp_dev *dev, unsigned profile, unsigned nx, unsigned ny, const uint32_t *coef,
                     uint32_t *lsc_error) {
    unsigned i;
    uint32_t err;
    if (!initialised(dev)) {
        return FX1_ISP_ESTATE;
    }
    if (profile > 2u || nx < 2u || nx > LSC_MESH_MAX || ny < 2u || ny > LSC_MESH_MAX || !coef) {
        return FX1_ISP_EINVAL;
    }
    const uint32_t nodes = nx | (ny << 8);
    /* Any change of the mesh size invalidates every profile (HAS p62), so it is
     * written only when it differs. */
    if ((RD(dev, FX1_ISP_LSC_MESH_NODES_OFFSET) & 0xFFFFu) != nodes) {
        WR(dev, FX1_ISP_LSC_MESH_NODES_OFFSET, nodes);
    }
    WR(dev, FX1_ISP_LSC_ERROR_OFFSET, 0xFFFFFFFFu);  /* W1C: start from a clean error state */
    WR(dev, FX1_ISP_LSC_LOAD_CTRL_OFFSET, profile | FX1_ISP_LSC_LOAD_CTRL_LOAD_BEGIN_MASK);
    err = RD(dev, FX1_ISP_LSC_ERROR_OFFSET);
    if (err) {  /* e.g. ACTIVE_LOAD_REJECT: the target is the active profile, valid or not (MAS Table 31) */
        if (lsc_error) {
            *lsc_error = err;
        }
        return FX1_ISP_EIO;
    }
    for (i = 0; i < 4u * nx * ny; ++i) {
        WR(dev, FX1_ISP_LSC_COEF_DATA_OFFSET, coef[i]);
    }
    WR(dev, FX1_ISP_LSC_LOAD_CTRL_OFFSET, profile | FX1_ISP_LSC_LOAD_CTRL_LOAD_VALIDATE_MASK);
    err = RD(dev, FX1_ISP_LSC_ERROR_OFFSET);
    if (lsc_error) {
        *lsc_error = err;
    }
    if (err & ~FX1_ISP_LSC_ERROR_COEF_RANGE_MASK) {  /* COEF_RANGE flags but does not block (ALG-LSC-03) */
        return FX1_ISP_EIO;
    }
    return (RD(dev, FX1_ISP_LSC_PROFILE_STATUS_OFFSET) & (1u << profile)) ? FX1_ISP_OK : FX1_ISP_EIO;
}

int fx1_isp_lsc_select(fx1_isp_dev *dev, unsigned profile) {
    if (!initialised(dev)) {
        return FX1_ISP_ESTATE;
    }
    if (profile > 2u) {
        return FX1_ISP_EINVAL;
    }
    WR(dev, FX1_ISP_LSC_ERROR_OFFSET, FX1_ISP_LSC_ERROR_PROFILE_SEL_REJECT_MASK);
    WR(dev, FX1_ISP_LSC_PROFILE_SEL_OFFSET, profile);
    return (RD(dev, FX1_ISP_LSC_ERROR_OFFSET) & FX1_ISP_LSC_ERROR_PROFILE_SEL_REJECT_MASK) ? FX1_ISP_EIO : FX1_ISP_OK;
}

void fx1_isp_ccm_set(fx1_isp_dev *dev, const int16_t m[9], const int16_t ofs[3], int enable) {
    unsigned i;
    const uint32_t en = enable ? FX1_ISP_CCM_CTRL_EN_MASK : 0u;
    if (!initialised(dev) || !m || !ofs) {
        return;
    }
    /* The block captures the registers at each accepted SOF; UPDATED is a
     * compatibility field with no effect (CSR/MAS 07/10, owner Q1). Nothing
     * protects a multi-register update: a SOF between two writes captures a
     * mixed set, so the caller must finish before the next frame starts.
     * The coefficients go first, the enable last. */
    for (i = 0; i < 9u; ++i) {
        WR(dev, FX1_ISP_CCM_CRR_OFFSET + 4u * i, (uint32_t)m[i] & 0xFFFu);
    }
    for (i = 0; i < 3u; ++i) {
        WR(dev, FX1_ISP_CCM_OFS_R_OFFSET + 4u * i, (uint32_t)ofs[i] & 0xFFFu);
    }
    WR(dev, FX1_ISP_CCM_CTRL_OFFSET, en | FX1_ISP_CCM_CTRL_UPDATED_MASK);
}

void fx1_isp_aec_configure(fx1_isp_dev *dev, const fx1_isp_aec_cfg *c, int enable) {
    if (!initialised(dev) || !c) {
        return;
    }
    WR(dev, FX1_ISP_AEC_ZONE_CFG_OFFSET, (c->nx & 0x3Fu) | ((c->ny & 0x1Fu) << 8));
    WR(dev, FX1_ISP_AEC_ZONE_SIZE_OFFSET, (c->zone_w & 0xFFFu) | ((c->zone_h & 0xFFFu) << 16));
    WR(dev, FX1_ISP_AEC_SAMPLE_CLIP_OFFSET, (c->clip_min & 0xFFFu) | ((c->clip_max & 0xFFFu) << 16));
    WR(dev, FX1_ISP_AEC_THRESH_OFFSET, (c->th_ue & 0xFFFu) | ((c->th_oe & 0xFFFu) << 16));
    WR(dev, FX1_ISP_AEC_CONTEXT_ID_OFFSET, c->context_id);
    WR(dev, FX1_ISP_AEC_CTRL_OFFSET, (enable ? FX1_ISP_AEC_CTRL_EN_MASK : 0u) | FX1_ISP_AEC_CTRL_COMMIT_MASK);
}

int fx1_isp_start(fx1_isp_dev *dev, const fx1_isp_buffers *b) {
    unsigned k;
    if (!initialised(dev)) {
        return FX1_ISP_ESTATE;
    }
    if (dev->state == FX1_ISP_STATE_STREAMING) {
        /* Draining does not reset the hardware's buffer rotation. START must
         * follow STOP, which resets both hardware and software pointers. */
        return dev->in_flight ? FX1_ISP_EBUSY : FX1_ISP_ESTATE;
    }
    if (!b || b->max_burst_beats < 1u || b->max_burst_beats > 256u) {
        return FX1_ISP_EINVAL;
    }
    const uint32_t width = RD(dev, FX1_ISP_COMMON_FRAME_WIDTH_OFFSET) & FX1_ISP_COMMON_FRAME_WIDTH_H_ACTIVE_MASK;
    if (b->in_stride < 2u * width) {
        return FX1_ISP_EINVAL;
    }
    /* HAS §9.1: the pipeline is enabled before the DMA (DEC-20: with ISP_EN = 0
     * the output geometry is 0x0 and the ODMA refuses to start). */
    const uint32_t old_isp_en = RD(dev, FX1_ISP_COMMON_CTRL_OFFSET) & FX1_ISP_COMMON_CTRL_ISP_EN_MASK;
    WR(dev, FX1_ISP_COMMON_CTRL_OFFSET, FX1_ISP_COMMON_CTRL_ISP_EN_MASK);
    /* Valid output geometry before the output DMA captures it (MAS §11.6 step 5). */
    delay(dev, RESIZER_SETTLE_CYCLES);
    const uint32_t out_width = RD(dev, FX1_ISP_RESIZER_OUT_W_OFFSET);
    const uint32_t out_height = RD(dev, FX1_ISP_RESIZER_OUT_H_OFFSET);
    if (b->y_stride < out_width || b->uv_stride < out_width) {
        WR(dev, FX1_ISP_COMMON_CTRL_OFFSET, old_isp_en);  /* failed start has no lasting enable */
        return FX1_ISP_EINVAL;
    }
    dev->out_width = out_width;
    dev->out_height = out_height;
    WR(dev, FX1_ISP_COMMON_IRQ_EN_OFFSET, FX1_ISP_COMMON_IRQ_EN_FRAME_DONE_EN_MASK | FX1_ISP_COMMON_IRQ_EN_ERROR_EN_MASK |
                                              FX1_ISP_COMMON_IRQ_EN_STATS_READY_EN_MASK);
    /* IDMA_DONE as well: it is when a frame whose output failed becomes
     * certainly lost (settle_losses). CFG_ERROR: an illegal start
     * configuration (MAS Table 162), reported in fx1_isp_events.dma. */
    WR(dev, FX1_ISP_DMA_IRQ_EN_OFFSET, FX1_ISP_DMA_IRQ_EN_IRQ_EN_ODMA_DONE_BIT | FX1_ISP_DMA_IRQ_EN_IRQ_EN_AXI_ERROR_BIT |
                                           FX1_ISP_DMA_IRQ_EN_IRQ_EN_ODMA_OVERFLOW_BIT |
                                           FX1_ISP_DMA_IRQ_EN_IRQ_EN_IDMA_DONE_BIT |
                                           FX1_ISP_DMA_IRQ_EN_IRQ_EN_CFG_ERROR_BIT);
    WR(dev, FX1_ISP_IDMA_STRIDE_OFFSET, b->in_stride);
    WR(dev, FX1_ISP_ODMA_Y_STRIDE_OFFSET, b->y_stride);
    WR(dev, FX1_ISP_ODMA_UV_STRIDE_OFFSET, b->uv_stride);
    for (k = 0; k < FX1_ISP_NUM_BUFFERS; ++k) {
        write_addr(dev, FX1_ISP_IDMA_BUF_ADDR_L0_OFFSET + 8u * k, b->in[k]);
        write_addr(dev, FX1_ISP_ODMA_Y_ADDR_L0_OFFSET + 16u * k, b->y[k]);
        write_addr(dev, FX1_ISP_ODMA_UV_ADDR_L0_OFFSET + 16u * k, b->uv[k]);
    }
    /* The engines are enabled with the first queued frame, so a stream does
     * not begin with an underrun report (M4-R5). */
    dev->dma_ctrl = ((b->max_burst_beats - 1u) << FX1_ISP_DMA_CTRL_MAX_BURST_M1_SHIFT) | FX1_ISP_DMA_CTRL_IDMA_EN_MASK |
                    FX1_ISP_DMA_CTRL_ODMA_EN_MASK;
    dev->started = 0;
    restart_rotation(dev);
    dev->state = FX1_ISP_STATE_STREAMING;
    return FX1_ISP_OK;
}

unsigned fx1_isp_next_input(const fx1_isp_dev *dev) { return dev->in_next; }

int fx1_isp_input_ready(fx1_isp_dev *dev) {
    if (!streaming(dev)) {
        return 0;
    }
    settle_losses(dev);  /* a loss decided now frees its in-flight slot */
    return dev->in_flight < FX1_ISP_NUM_BUFFERS &&
           !(RD(dev, FX1_ISP_IDMA_BUF_VALID_OFFSET) & (1u << dev->in_next));
}

static int queue_frame(fx1_isp_dev *dev, uint32_t *seq, const uint32_t *meta);

int fx1_isp_queue_frame(fx1_isp_dev *dev) { return fx1_isp_queue_frame_ex(dev, NULL); }

int fx1_isp_queue_frame_ex(fx1_isp_dev *dev, uint32_t *seq) { return queue_frame(dev, seq, NULL); }

int fx1_isp_queue_frame_meta(fx1_isp_dev *dev, uint32_t tag, uint32_t ts, uint32_t *seq) {
    const uint32_t meta[2] = {tag, ts};
    return queue_frame(dev, seq, meta);
}

static int queue_frame(fx1_isp_dev *dev, uint32_t *seq, const uint32_t *meta) {
    if (!streaming(dev)) {
        return FX1_ISP_ESTATE;
    }
    /* An early ODMA failure can release an output slot while IDMA is still
     * reading the corresponding input. Never re-arm a VALID input. */
    if (!fx1_isp_input_ready(dev)) {
        return FX1_ISP_EBUSY;
    }
    /* The input's tag and capture timestamp, before IDMA_BUF_VALID (MAS
     * Table 160): the IDMA captures them when it starts the frame. */
    if (meta) {
        WR(dev, FX1_ISP_IDMA_BUF_TAG0_OFFSET + 4u * dev->in_next, meta[0]);
        WR(dev, FX1_ISP_IDMA_BUF_TS0_OFFSET + 4u * dev->in_next, meta[1]);
    }
    /* Output first, so the ODMA is armed when the frame arrives. */
    WR(dev, FX1_ISP_ODMA_BUF_FREE_OFFSET, 1u << dev->out_next);
    WR(dev, FX1_ISP_IDMA_BUF_VALID_OFFSET, 1u << dev->in_next);
    dev->queue[dev->in_flight].seq = dev->next_seq;
    dev->queue[dev->in_flight].input = dev->in_next;
    dev->queue[dev->in_flight].odma_failed = 0;
    if (seq) {
        *seq = dev->next_seq;
    }
    ++dev->next_seq;
    dev->out_next = (dev->out_next + 1u) % FX1_ISP_NUM_BUFFERS;
    dev->in_next = (dev->in_next + 1u) % FX1_ISP_NUM_BUFFERS;
    ++dev->in_flight;
    if (!dev->started) {
        WR(dev, FX1_ISP_DMA_CTRL_OFFSET, dev->dma_ctrl);
        dev->started = 1;
    }
    return FX1_ISP_OK;
}

void fx1_isp_irq(fx1_isp_dev *dev, fx1_isp_events *ev) {
    unsigned k, n = 0;
    if (!ev) {
        return;
    }
    ev->common = ev->dma = ev->dma_err = 0;
    ev->frames_done = ev->frames_lost = 0;
    if (!initialised(dev)) {
        return;
    }
    ev->common = RD(dev, FX1_ISP_COMMON_IRQ_STATUS_OFFSET);
    ev->dma = RD(dev, FX1_ISP_DMA_IRQ_STAT_OFFSET);
    ev->dma_err = RD(dev, FX1_ISP_DMA_ERR_OFFSET);
    /* W1C, per bit: a source that fires again between the read and the
     * write is set again by hardware (hardware set wins, HAS Table 7-10). */
    WR(dev, FX1_ISP_COMMON_IRQ_STATUS_OFFSET, ev->common);
    WR(dev, FX1_ISP_DMA_IRQ_STAT_OFFSET, ev->dma);
    if (ev->dma_err & FX1_ISP_DMA_ERR_ERR_AXI_RD_BIT) {
        stats_untrust(dev, 1);  /* an input frame was aborted: zone data may be partial */
    }
    if (dev->state == FX1_ISP_STATE_STREAMING) {
        settle_losses(dev);
    }
    ev->frames_lost = dev->lost_count;
    const uint32_t done = RD(dev, FX1_ISP_ODMA_BUF_DONE_OFFSET);
    for (k = 0; k < dev->in_flight; ++k) {  /* completed buffers, in rotation order */
        if (!(done & (1u << ((dev->out_done + k) % FX1_ISP_NUM_BUFFERS)))) {
            break;
        }
        ++n;
    }
    ev->frames_done = n;
}

int fx1_isp_next_done(fx1_isp_dev *dev, unsigned *index) { return fx1_isp_next_done_ex(dev, index, NULL); }

int fx1_isp_next_done_ex(fx1_isp_dev *dev, unsigned *index, uint32_t *seq) {
    return fx1_isp_next_done_meta(dev, index, seq, NULL);
}

int fx1_isp_next_done_meta(fx1_isp_dev *dev, unsigned *index, uint32_t *seq, fx1_isp_frame_meta *meta) {
    if (!streaming(dev) || !index) {
        return streaming(dev) ? FX1_ISP_EINVAL : FX1_ISP_ESTATE;
    }
    settle_losses(dev);  /* positions must be final before an output is matched to a frame */
    if (!dev->in_flight || !(RD(dev, FX1_ISP_ODMA_BUF_DONE_OFFSET) & (1u << dev->out_done))) {
        return FX1_ISP_ENODATA;
    }
    *index = dev->out_done;
    if (meta) {
        /* Published with the buffer at completion; stable until software
         * frees this buffer again (MAS Table 160). */
        meta->tag = RD(dev, FX1_ISP_ODMA_BUF_TAG0_OFFSET + 4u * *index);
        meta->ts = RD(dev, FX1_ISP_ODMA_BUF_TS0_OFFSET + 4u * *index);
        meta->sof_ts = RD(dev, FX1_ISP_ODMA_BUF_SOF_TS0_OFFSET + 4u * *index);
        meta->eof_ts = RD(dev, FX1_ISP_ODMA_BUF_EOF_TS0_OFFSET + 4u * *index);
    }
    WR(dev, FX1_ISP_ODMA_BUF_DONE_OFFSET, 1u << dev->out_done);  /* returned to software */
    /* A W1C on the core clock on which the hardware set the bit loses
     * (hardware set wins): possible when CSR accesses are shorter than a
     * core clock. The ODMA never clears DONE, and this buffer is not free, so
     * a bit still set is that same completion: clear it again on a later
     * clock. Otherwise a stale DONE would make this buffer look complete
     * when it is queued again (review U6-R3). */
    if (RD(dev, FX1_ISP_ODMA_BUF_DONE_OFFSET) & (1u << dev->out_done)) {
        delay(dev, 1u);
        WR(dev, FX1_ISP_ODMA_BUF_DONE_OFFSET, 1u << dev->out_done);
    }
    dev->out_done = (dev->out_done + 1u) % FX1_ISP_NUM_BUFFERS;
    const fx1_isp_frame f = queue_remove(dev, 0);
    if (seq) {
        *seq = f.seq;
    }
    return FX1_ISP_OK;
}

uint32_t fx1_isp_frame_counter(fx1_isp_dev *dev) {
    return initialised(dev) ? RD(dev, FX1_ISP_FRAME_COUNTER_OFFSET) : 0u;
}

int fx1_isp_recover(fx1_isp_dev *dev, uint32_t dma_err) { return fx1_isp_recover_ex(dev, dma_err, NULL); }

int fx1_isp_recover_ex(fx1_isp_dev *dev, uint32_t dma_err, fx1_isp_recovery *r) {
    fx1_isp_recovery info = {0, 0, 0, 0, 0, 0, 0};
    unsigned k;
    if (!streaming(dev)) {
        return FX1_ISP_ESTATE;
    }
    /* Only errors still flagged in hardware: recover clears what it handles
     * (W1C), so a repeated call with an old interrupt snapshot is a no-op. */
    const uint32_t pending = dma_err & RD(dev, FX1_ISP_DMA_ERR_OFFSET) &
                             (FX1_ISP_DMA_ERR_ERR_AXI_RD_BIT | FX1_ISP_DMA_ERR_ERR_AXI_WR_BIT);
    if (pending & FX1_ISP_DMA_ERR_ERR_AXI_WR_BIT) {
        /* The output buffer was neither completed nor left free, and the ODMA
         * kept its rotation position: free the same index again. The failed
         * buffer is the first in-flight one, in rotation order from the next
         * one to collect, whose DONE bit is clear: frames before it may be
         * DONE and not yet collected when the handler runs late (M5-R5).
         * Whether the frame is lost is decided by settle_losses. */
        const uint32_t done = RD(dev, FX1_ISP_ODMA_BUF_DONE_OFFSET);
        for (k = 0; k < dev->in_flight; ++k) {
            if (!(done & (1u << ((dev->out_done + k) % FX1_ISP_NUM_BUFFERS)))) {
                break;
            }
        }
        WR(dev, FX1_ISP_DMA_ERR_OFFSET, FX1_ISP_DMA_ERR_ERR_AXI_WR_BIT | FX1_ISP_DMA_ERR_ERR_ODMA_OVERFLOW_BIT);
        if (k < dev->in_flight) {
            info.odma_output = (dev->out_done + k) % FX1_ISP_NUM_BUFFERS;
            WR(dev, FX1_ISP_ODMA_BUF_FREE_OFFSET, 1u << info.odma_output);
            dev->queue[k].odma_failed = 1;
            info.odma_failed = 1;
            info.odma_seq = dev->queue[k].seq;
        }
        info.handled |= FX1_ISP_DMA_ERR_ERR_AXI_WR_BIT;
    }
    if (pending & FX1_ISP_DMA_ERR_ERR_AXI_RD_BIT) {
        /* The IDMA released the input it was reading and kept its rotation
         * position: re-arm the same index (DEC-19). The aborted frame left
         * partial zone/histogram data behind (M3-A7). */
        const int pos = idma_failed_pos(dev, RD(dev, FX1_ISP_IDMA_BUF_VALID_OFFSET));
        stats_untrust(dev, 1);
        if (pos >= 0) {
            fx1_isp_frame *f = &dev->queue[pos];
            f->odma_failed = 0;  /* retried from its input: its output will be produced */
            dev->in_failed = f->input;
            info.idma_rearmed = 1;
            info.idma_input = f->input;
            info.idma_seq = f->seq;
            /* Losses before it are settled while it is still the failed one. */
            settle_losses(dev);
            WR(dev, FX1_ISP_DMA_ERR_OFFSET, FX1_ISP_DMA_ERR_ERR_AXI_RD_BIT);
            WR(dev, FX1_ISP_IDMA_BUF_VALID_OFFSET, 1u << info.idma_input);
        } else {
            WR(dev, FX1_ISP_DMA_ERR_OFFSET, FX1_ISP_DMA_ERR_ERR_AXI_RD_BIT);
        }
        info.handled |= FX1_ISP_DMA_ERR_ERR_AXI_RD_BIT;
    }
    settle_losses(dev);
    if (r) {
        *r = info;
    }
    return info.handled ? FX1_ISP_OK : FX1_ISP_ENODATA;
}

int fx1_isp_next_lost(fx1_isp_dev *dev, uint32_t *seq, unsigned *input) {
    unsigned i;
    if (!streaming(dev)) {
        return FX1_ISP_ESTATE;
    }
    settle_losses(dev);
    if (!dev->lost_count) {
        return FX1_ISP_ENODATA;
    }
    if (seq) {
        *seq = dev->lost[0].seq;
    }
    if (input) {
        *input = dev->lost[0].input;
    }
    for (i = 0; i + 1u < dev->lost_count; ++i) {
        dev->lost[i] = dev->lost[i + 1u];
    }
    --dev->lost_count;
    return FX1_ISP_OK;
}

int fx1_isp_get_progress(fx1_isp_dev *dev, fx1_isp_progress *p) {
    if (!initialised(dev) || !p) {
        return initialised(dev) ? FX1_ISP_EINVAL : FX1_ISP_ESTATE;
    }
    p->idma_frames = RD(dev, FX1_ISP_IDMA_FRAME_COUNT_OFFSET);
    p->odma_frames = RD(dev, FX1_ISP_ODMA_FRAME_COUNT_OFFSET);
    p->in_flight = dev->in_flight;
    p->dma_err = RD(dev, FX1_ISP_DMA_ERR_OFFSET);
    p->pipeline_busy = (RD(dev, FX1_ISP_COMMON_STATUS_OFFSET) & FX1_ISP_COMMON_STATUS_BUSY_MASK) != 0;
    p->isp_enabled = (RD(dev, FX1_ISP_COMMON_CTRL_OFFSET) & FX1_ISP_COMMON_CTRL_ISP_EN_MASK) != 0;
    return FX1_ISP_OK;
}

/* ---- Statistics --------------------------------------------------------------- */
static uint32_t sel_read(fx1_isp_dev *dev, uint32_t sel_off, uint32_t sel, uint32_t data_off) {
    WR(dev, sel_off, sel);
    return RD(dev, data_off);
}

int fx1_isp_read_aec_global(fx1_isp_dev *dev, fx1_isp_aec_global *o) {
    int t;
    unsigned c;
    if (!initialised(dev) || !o) {
        return initialised(dev) ? FX1_ISP_EINVAL : FX1_ISP_ESTATE;
    }
    for (t = 0; t < STATS_RETRIES; ++t) {  /* read-tag-read, HAS §6.21.9 */
        const uint32_t id = RD(dev, FX1_ISP_AEC_FRAME_ID_OFFSET);
        for (c = 0; c < 4u; ++c) {
            WR(dev, FX1_ISP_AEC_CHANNEL_SEL_OFFSET, c);
            const uint32_t lo = RD(dev, FX1_ISP_AEC_GLOBAL_SUM_LO_OFFSET);
            const uint32_t hi = RD(dev, FX1_ISP_AEC_GLOBAL_SUM_HI_OFFSET);
            o->sum[c] = ((uint64_t)hi << 32) | lo;
            o->count[c] = RD(dev, FX1_ISP_AEC_GLOBAL_COUNT_OFFSET);
        }
        o->context_id = RD(dev, FX1_ISP_AEC_RESULT_CONTEXT_ID_OFFSET);
        if (RD(dev, FX1_ISP_AEC_FRAME_ID_OFFSET) == id) {
            o->frame_id = id;
            return FX1_ISP_OK;
        }
    }
    return FX1_ISP_EAGAIN;
}

int fx1_isp_read_aec_zones(fx1_isp_dev *dev, unsigned first, unsigned n, fx1_isp_aec_zone *zones, uint32_t hist[64],
                           uint32_t *frame_id) {
    unsigned i, c;
    if (!initialised(dev)) {
        return FX1_ISP_ESTATE;
    }
    if (n && !zones) {
        return FX1_ISP_EINVAL;
    }
    /* The zone and histogram memory is rewritten from the next enabled SOF
     * (DEC-30): FRAME_ID alone cannot detect that, so busy must be 0 before
     * and after the read (ALG-STAT-02). */
    const uint32_t id = RD(dev, FX1_ISP_AEC_FRAME_ID_OFFSET);
    if (RD(dev, FX1_ISP_AEC_STATUS_OFFSET) & FX1_ISP_AEC_STATUS_BUSY_MASK) {
        return FX1_ISP_EAGAIN;
    }
    /* A caller may poll statistics before servicing the error IRQ. The
     * hardware error is visible even when the driver has not marked it yet. */
    if (RD(dev, FX1_ISP_DMA_ERR_OFFSET) & FX1_ISP_DMA_ERR_ERR_AXI_RD_BIT) {
        stats_untrust(dev, 1);
        return FX1_ISP_ENODATA;
    }
    if (dev->stats_untrusted) {
        if (id == dev->stats_mark) {
            return FX1_ISP_ENODATA;  /* partial/cleared data of an aborted frame or a reset */
        }
        dev->stats_untrusted = 0;    /* a new frame was published since */
    }
    for (i = 0; i < n; ++i) {
        fx1_isp_aec_zone *z = &zones[i];
        select_word(dev, FX1_ISP_AEC_ZONE_ADDR_OFFSET, first + i);
        const uint32_t oeue = RD(dev, FX1_ISP_AEC_ZONE_GREEN_OE_UE_OFFSET);
        const uint32_t mm = RD(dev, FX1_ISP_AEC_ZONE_GREEN_MIN_MAX_OFFSET);
        z->green_ue = oeue & 0xFFFFu;
        z->green_oe = oeue >> 16;
        z->green_min = mm & 0xFFFu;
        z->green_max = (mm >> 12) & 0xFFFu;
        for (c = 0; c < 4u; ++c) {
            WR(dev, FX1_ISP_AEC_CHANNEL_SEL_OFFSET, c);
            z->sum[c] = RD(dev, FX1_ISP_AEC_ZONE_SUM_OFFSET);
            z->count[c] = RD(dev, FX1_ISP_AEC_ZONE_COUNT_OFFSET);
        }
    }
    if (hist) {
        for (i = 0; i < 64u; ++i) {
            select_word(dev, FX1_ISP_AEC_HIST_ADDR_OFFSET, i);
            hist[i] = RD(dev, FX1_ISP_AEC_HIST_DATA_OFFSET);
        }
    }
    if (RD(dev, FX1_ISP_DMA_ERR_OFFSET) & FX1_ISP_DMA_ERR_ERR_AXI_RD_BIT) {
        stats_untrust(dev, 1);
        return FX1_ISP_ENODATA;
    }
    if ((RD(dev, FX1_ISP_AEC_STATUS_OFFSET) & FX1_ISP_AEC_STATUS_BUSY_MASK) || RD(dev, FX1_ISP_AEC_FRAME_ID_OFFSET) != id) {
        return FX1_ISP_EAGAIN;
    }
    if (frame_id) {
        *frame_id = id;
    }
    return FX1_ISP_OK;
}

/* ---- AWB result ownership (MAS §6.22.4) ---- */

static uint64_t rd_split(fx1_isp_dev *dev, uint32_t hi, uint32_t lo) {
    return ((uint64_t)(RD(dev, hi) & 0x7u) << 32) | RD(dev, lo);
}

/* The lease is still valid. If not, a COMMON soft reset the driver did not
 * issue has cleared every AWB result (MAS p248): drop the lease and the
 * cached result of the compatibility reader. */
static int awb_lease_alive(fx1_isp_dev *dev) {
    if (RD(dev, FX1_ISP_AWB_RESULT_STATUS_OFFSET) & FX1_ISP_AWB_RESULT_STATUS_READ_VALID_MASK) {
        return 1;
    }
    dev->awb_leased = 0;
    dev->awb_last_valid = 0;
    return 0;
}

int fx1_isp_awb_acquire(fx1_isp_dev *dev, fx1_isp_awb_lease *out) {
    uint32_t st;
    if (!initialised(dev)) {
        return FX1_ISP_ESTATE;
    }
    if (dev->awb_leased) {
        return FX1_ISP_EBUSY;
    }
    if (!(RD(dev, FX1_ISP_AWB_RESULT_STATUS_OFFSET) & FX1_ISP_AWB_RESULT_STATUS_READY_COUNT_MASK)) {
        return FX1_ISP_ENODATA;
    }
    /* READ_VALID (= LEASED) rises one core clock after ACQUIRE (MAS
     * §6.22.4.3): wait that clock before confirming. Then READ_VALID = 0
     * means no bank was taken (a reset in between); nothing is owned. */
    select_word(dev, FX1_ISP_AWB_RESULT_CTRL_OFFSET, FX1_ISP_AWB_RESULT_CTRL_ACQUIRE_MASK);
    st = RD(dev, FX1_ISP_AWB_RESULT_STATUS_OFFSET);
    if (!(st & FX1_ISP_AWB_RESULT_STATUS_READ_VALID_MASK)) {
        return FX1_ISP_ENODATA;
    }
    dev->awb_leased = 1;
    dev->awb_lease.bank = (st & FX1_ISP_AWB_RESULT_STATUS_READ_BANK_MASK) >> FX1_ISP_AWB_RESULT_STATUS_READ_BANK_SHIFT;
    dev->awb_lease.nx = (st & FX1_ISP_AWB_RESULT_STATUS_READ_NUM_ZONE_X_MASK) >> FX1_ISP_AWB_RESULT_STATUS_READ_NUM_ZONE_X_SHIFT;
    dev->awb_lease.ny = (st & FX1_ISP_AWB_RESULT_STATUS_READ_NUM_ZONE_Y_MASK) >> FX1_ISP_AWB_RESULT_STATUS_READ_NUM_ZONE_Y_SHIFT;
    dev->awb_lease.frame_id = RD(dev, FX1_ISP_AWB_FRAME_ID_OFFSET);
    dev->awb_lease.context_id = RD(dev, FX1_ISP_AWB_RESULT_CONTEXT_ID_OFFSET);
    if (out) {
        *out = dev->awb_lease;
    }
    return FX1_ISP_OK;
}

int fx1_isp_awb_read_global(fx1_isp_dev *dev, fx1_isp_awb_global *o) {
    if (!initialised(dev)) {
        return FX1_ISP_ESTATE;
    }
    if (!o) {
        return FX1_ISP_EINVAL;
    }
    if (!dev->awb_leased) {
        return FX1_ISP_ESTATE;
    }
    o->sum[0] = rd_split(dev, FX1_ISP_AWB_GLOBAL_SUM_R_H_OFFSET, FX1_ISP_AWB_GLOBAL_SUM_R_L_OFFSET);
    o->sum[1] = rd_split(dev, FX1_ISP_AWB_GLOBAL_SUM_G_H_OFFSET, FX1_ISP_AWB_GLOBAL_SUM_G_L_OFFSET);
    o->sum[2] = rd_split(dev, FX1_ISP_AWB_GLOBAL_SUM_B_H_OFFSET, FX1_ISP_AWB_GLOBAL_SUM_B_L_OFFSET);
    o->count = RD(dev, FX1_ISP_AWB_GLOBAL_COUNT_OFFSET);
    o->frame_id = dev->awb_lease.frame_id;
    o->context_id = dev->awb_lease.context_id;
    /* The leased bank is immutable; only a reset can end the lease. */
    return awb_lease_alive(dev) ? FX1_ISP_OK : FX1_ISP_ENODATA;
}

int fx1_isp_awb_read_zones(fx1_isp_dev *dev, unsigned first, unsigned n, fx1_isp_awb_zone *zones) {
    unsigned i;
    if (!initialised(dev)) {
        return FX1_ISP_ESTATE;
    }
    if (n && !zones) {
        return FX1_ISP_EINVAL;
    }
    if (!dev->awb_leased) {
        return FX1_ISP_ESTATE;
    }
    if (first > dev->awb_lease.nx * dev->awb_lease.ny || n > dev->awb_lease.nx * dev->awb_lease.ny - first) {
        return FX1_ISP_EINVAL;  /* beyond the captured grid */
    }
    for (i = 0; i < n; ++i) {
        fx1_isp_awb_zone *z = &zones[i];
        select_word(dev, FX1_ISP_AWB_ZONE_ADDR_OFFSET, first + i);
        z->count = RD(dev, FX1_ISP_AWB_ZONE_COUNT_OFFSET);
        z->sum[0] = rd_split(dev, FX1_ISP_AWB_ZONE_SUM_R_H_OFFSET, FX1_ISP_AWB_ZONE_SUM_R_L_OFFSET);
        z->sum[1] = rd_split(dev, FX1_ISP_AWB_ZONE_SUM_G_H_OFFSET, FX1_ISP_AWB_ZONE_SUM_G_L_OFFSET);
        z->sum[2] = rd_split(dev, FX1_ISP_AWB_ZONE_SUM_B_H_OFFSET, FX1_ISP_AWB_ZONE_SUM_B_L_OFFSET);
    }
    return awb_lease_alive(dev) ? FX1_ISP_OK : FX1_ISP_ENODATA;
}

int fx1_isp_awb_release(fx1_isp_dev *dev) {
    if (!initialised(dev)) {
        return FX1_ISP_ESTATE;
    }
    if (!dev->awb_leased) {
        return FX1_ISP_ESTATE;
    }
    /* A bank released on an SOF edge is eligible only from the next edge
     * (MAS §6.22.4.3): wait that clock, so a frame the caller starts next can
     * use the bank. */
    select_word(dev, FX1_ISP_AWB_RESULT_CTRL_OFFSET, FX1_ISP_AWB_RESULT_CTRL_RELEASE_MASK);
    dev->awb_leased = 0;
    return FX1_ISP_OK;
}

int fx1_isp_awb_report(fx1_isp_dev *dev, fx1_isp_awb_status *o, int clear) {
    uint32_t st;
    if (!initialised(dev)) {
        return FX1_ISP_ESTATE;
    }
    if (!o) {
        return FX1_ISP_EINVAL;
    }
    st = RD(dev, FX1_ISP_AWB_RESULT_STATUS_OFFSET);
    o->ready_count = (st & FX1_ISP_AWB_RESULT_STATUS_READY_COUNT_MASK) >> FX1_ISP_AWB_RESULT_STATUS_READY_COUNT_SHIFT;
    o->leased = (st & FX1_ISP_AWB_RESULT_STATUS_LEASED_MASK) != 0;
    o->write_busy = (st & FX1_ISP_AWB_RESULT_STATUS_WRITE_BUSY_MASK) != 0;
    o->overrun = (st & FX1_ISP_AWB_RESULT_STATUS_OVERRUN_MASK) != 0;
    o->drop_count = RD(dev, FX1_ISP_AWB_DROP_COUNT_OFFSET);
    if (clear) {
        WR(dev, FX1_ISP_AWB_RESULT_CTRL_OFFSET, FX1_ISP_AWB_RESULT_CTRL_CLEAR_REPORT_MASK);
    }
    return FX1_ISP_OK;
}

int fx1_isp_read_awb_global(fx1_isp_dev *dev, fx1_isp_awb_global *o) {
    unsigned guard;
    if (!initialised(dev) || !o) {
        return initialised(dev) ? FX1_ISP_EINVAL : FX1_ISP_ESTATE;
    }
    if (dev->awb_leased) {
        return FX1_ISP_EBUSY;  /* the caller holds a result through the explicit API */
    }
    /* Drain every ready result, oldest first, keeping the newest. At most two
     * banks exist; the guard bounds the loop if frames keep completing. */
    for (guard = 0; guard < 4u && fx1_isp_awb_acquire(dev, NULL) == FX1_ISP_OK; ++guard) {
        fx1_isp_awb_global g;
        const int rc = fx1_isp_awb_read_global(dev, &g);
        if (dev->awb_leased) {
            fx1_isp_awb_release(dev);
        }
        if (rc == FX1_ISP_OK) {
            dev->awb_last = g;
            dev->awb_last_valid = 1;
        }
    }
    if (!dev->awb_last_valid) {
        return FX1_ISP_ENODATA;
    }
    *o = dev->awb_last;
    return FX1_ISP_OK;
}

int fx1_isp_read_af(fx1_isp_dev *dev, fx1_isp_af_scores *o) {
    int t;
    unsigned z;
    if (!initialised(dev) || !o) {
        return initialised(dev) ? FX1_ISP_EINVAL : FX1_ISP_ESTATE;
    }
    for (t = 0; t < STATS_RETRIES; ++t) {
        const uint32_t id = RD(dev, FX1_ISP_AF_FRAME_ID_OFFSET);
        for (z = 0; z < 16u; ++z) {
            o->fv[z] = sel_read(dev, FX1_ISP_AF_STAT_ADDR_OFFSET, z, FX1_ISP_AF_STAT_DATA_OFFSET);
        }
        o->score_valid = (RD(dev, FX1_ISP_AF_STATUS_OFFSET) & FX1_ISP_AF_STATUS_SCORE_VALID_MASK) != 0;
        o->context_id = RD(dev, FX1_ISP_AF_RESULT_CONTEXT_ID_OFFSET);
        if (RD(dev, FX1_ISP_AF_FRAME_ID_OFFSET) == id) {
            o->frame_id = id;
            return FX1_ISP_OK;
        }
    }
    return FX1_ISP_EAGAIN;
}
