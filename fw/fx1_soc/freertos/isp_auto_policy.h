/* SPDX-License-Identifier: Apache-2.0
 * Demo AWB/tone policy, not sensor calibration. No image-name or scene tags.
 * AWB sums are post-WB/pre-CCM. A bounded neutral-zone estimator corrects the
 * CURRENT gains. Prefer bright, non-clipped neutral zones; retain the base
 * when evidence is weak (dark, saturated, single-colour scene).
 */
#ifndef ISP_AUTO_POLICY_H
#define ISP_AUTO_POLICY_H
#include <stdint.h>
#define ISP_AUTO_POLICY_ID "neutral-v1"
typedef struct {
    uint64_t r[8], g[8], b[8];
    unsigned zones[8];
} isp_auto_meter;
typedef struct {
    uint32_t r, g, b, key, white, beta, blend, zones;
    int valid;
} isp_auto_choice;
static inline void isp_auto_add(isp_auto_meter *m, const uint64_t sum[3], uint32_t count)
{
    if (count < 64) return;
    const uint32_t r=(uint32_t)(sum[0]/count), g=(uint32_t)(sum[1]/count), b=(uint32_t)(sum[2]/count);
    if (g < 256 || g > 3400 || r > 3600 || b > 3600) return;
    if (r*100u < g*80u || r*100u > g*120u || b*100u < g*55u || b*100u > g*130u) return;
    const unsigned k=g/512u;
    m->r[k]+=r; m->g[k]+=g; m->b[k]+=b; ++m->zones[k];
}
static inline uint32_t isp_auto_gain(uint32_t old, uint64_t green, uint64_t colour)
{
    uint64_t v=((uint64_t)old*green+colour/2u)/colour;
    /* Bound correction to [0.625, 1.6] and legal demo range [1, 4]. */
    const uint32_t lo=old*5u/8u, hi=old*8u/5u;
    if (v<lo) v=lo;
    if (v>hi) v=hi;
    if (v<256u) v=256u;
    if (v>1024u) v=1024u;
    return (uint32_t)v;
}
static inline isp_auto_choice isp_auto_decide(const isp_auto_meter *m, uint32_t r, uint32_t g,
                                             uint32_t b, uint32_t key, uint32_t white, uint32_t beta)
{
    isp_auto_choice c={r,g,b,key,white,beta,0,0,0};
    uint64_t sr=0,sg=0,sb=0;
    /* Use the highest populated brightness bands; at least eight zones. */
    for (int k=7;k>=0;--k) {
        sr+=m->r[k]; sg+=m->g[k]; sb+=m->b[k]; c.zones+=m->zones[k];
        if (c.zones>=8) break;
    }
    if (c.zones<8 || !sr || !sg || !sb) return c;
    c.r=isp_auto_gain(r,sg,sr); c.b=isp_auto_gain(b,sg,sb); c.valid=1;
    /* Demo interpolation by WB B/R, not an illuminant classification.
     * Base look at B/R <= 1.0, accepted LED look at B/R >= 1.2.
     * Keep WB_GAIN_G and the base pipeline, and limit tone/EE overrides. */
    const uint32_t ratio=c.b*256u/c.r;
    c.blend=ratio<=256u ? 0u : ratio>=307u ? 256u : (ratio-256u)*256u/51u;
    /* Preserve the accepted base look at the daylight end; gradually adopt
     * the neutral estimate toward the LED end. This is a bounded demo
     * policy, not universal AWB or colour-temperature estimation. */
    c.r=(r*(256u-c.blend)+c.r*c.blend+128u)/256u;
    c.b=(b*(256u-c.blend)+c.b*c.blend+128u)/256u;
    c.key=(key*(256u-c.blend)+0xB0u*c.blend+128u)/256u;
    c.white=(white*(256u-c.blend)+0x180u*c.blend+128u)/256u;
    c.beta=(beta*(256u-c.blend)+0x6000u*c.blend+128u)/256u;
    return c;
}
#endif
