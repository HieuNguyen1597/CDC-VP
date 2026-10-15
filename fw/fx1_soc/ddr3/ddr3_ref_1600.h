/*
 * DDR3 reference image of the FX1 VP: NATIVE_R2 (R = 2 tCK per controller cycle),
 * DDR3-1600, 4 Gb x8 devices on the x32 bus, 2 ranks (4 GiB physical), linear map.
 * Values from the DDR3 programming guide (components/FX1_Components/ddr3/docs/
 * programming_guide.md, sections 4.2-4.5). test_fx1_ddr3_subsystem (case `refimage`)
 * checks them against the model's own reference image for this build.
 *
 * Plain C99, no dependency beyond the register header: usable from BootROM code.
 */
#ifndef FX1_DDR3_REF_1600_H
#define FX1_DDR3_REF_1600_H

#include <stdint.h>

#include "fx1_ddr3/fx1_ddr3_regs.h"

/* TIMING_SHADOW(i), tCK, in FX1_DDR3_T_* index order (programming guide §4.2). */
static const uint32_t ddr3_ref_timing[FX1_DDR3_TIMING_WORDS] = {
    11u,   /* T_RCD */
    11u,   /* T_RP */
    28u,   /* T_RAS */
    39u,   /* T_RC */
    5u,    /* T_RRD */
    24u,   /* T_FAW */
    4u,    /* T_CCD */
    6u,    /* T_RTP */
    12u,   /* T_WR */
    6u,    /* T_WTR */
    9u,    /* T_RTW */
    2u,    /* T_RTRS */
    208u,  /* T_RFC */
    6240u, /* T_REFI */
    11u,   /* CL */
    8u,    /* CWL */
    0u,    /* AL */
    2u,    /* PHY_WRITE_LATENCY */
    6u,    /* PHY_READ_LATENCY */
};

/* DFI shadow (programming guide §4.3). */
#define DDR3_REF_DFI_WR_ENABLE_DELAY 4u
#define DDR3_REF_DFI_RD_ENABLE_DELAY 13u
#define DDR3_REF_PHY_MAX_READ_RETURN 256u
#define DDR3_REF_DFI_ASSEMBLY_MARGIN 1u

/* INIT_RW waits in controller cycles = ceil(time / (R x tCK)) (programming guide §4.4). */
#define DDR3_REF_INIT_RESET_RELEASE 200000u /* 500 us */
#define DDR3_REF_INIT_CKE_LOW       4u      /* max(10 ns, 5 tCK) */
#define DDR3_REF_INIT_CKE_HIGH      108u    /* tXPR = tRFC + 10 ns */
#define DDR3_REF_INIT_DLL           256u    /* tDLLK = 512 tCK */
#define DDR3_REF_INIT_ZQ            256u    /* tZQinit = 512 tCK */

/* Topology (programming guide §4.5): 2 ranks, 16 row bits, 1024 words per row. */
#define DDR3_REF_RANKS    2u
#define DDR3_REF_ROW_BITS 16u
#define DDR3_REF_TOPO \
    FX1_DDR3_TOPO_VAL(DDR3_REF_RANKS, DDR3_REF_ROW_BITS, FX1_DDR3_TOPO_PAGE_1024, FX1_DDR3_TOPO_MAP_LINEAR)

#endif /* FX1_DDR3_REF_1600_H */
