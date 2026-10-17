#pragma once
/* Local offsets from the SoC WDT window; do not add 0x1000 again. */
#define FU3_WDT0_BASE 0x44050000u
#define FU3_WDT1_BASE 0x44080000u
#define FU3_WDT_LOCK 0x1000u
#define FU3_WDT_LOAD 0x1004u
#define FU3_WDT_VALUE 0x1008u
#define FU3_WDT_CONTROL 0x100cu
#define FU3_WDT_RAW 0x1010u
#define FU3_WDT_MASKED 0x1014u
#define FU3_WDT_CLEAR 0x1018u
#define FU3_WDT_DEBUG 0x101cu
#define FU3_WDT_UNLOCK_KEY 0x1acce551u
#define FU3_WDT_RESEN 1u
#define FU3_WDT_INTEN 2u
#define FU3_WDT_DBGEN 4u
#define FU3_WDT_RESDIV_SHIFT 3u
#define FU3_WDT_RESDIV_MASK 0x78u
