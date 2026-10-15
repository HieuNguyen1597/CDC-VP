/*
 * CPU and DMA atomics through DDR3 region aliases (review S5-R3; default preinit boot).
 *
 * Two FE regions of port 0: region 0 = [0x8000_0000, 0x9800_0000) and region 1 =
 * [0x9800_0000, 0xA000_0000). Every region maps its first byte to user offset 0 (L-13),
 * so 0x9C00_0000 + x and 0x8400_0000 + x name the same storage (user offset 0x0400_0000
 * + x). This image, its stacks and the DMA descriptors stay in region 0 below the alias.
 * The platform exclusive monitor must treat both names as one location:
 *
 *  A  the aliases really share data
 *  B  hart 1 stores through one name: hart 0's reservation through the other is voided
 *  C  SYS_DMA writes through one name: the reservation through the other is voided
 *  D  amoor.w through one name against DMA writes through the other: no DMA write is lost
 *     inside an AMO (the ctest also requires "device writes held" > 0: the brackets met)
 *
 * Fail codes: 0x6x.
 */
#include "atomic_common.h"
#include "ddr3_fw.h"

#define NAME_A(off) (0x84000000u + (off))
#define NAME_B(off) (0x9C000000u + (off))
#define TRIALS      64u
#define DMA_VALUE   0x80000000u

static volatile uint32_t go, done;

static void commit_regions(uint32_t size0, uint32_t base1, uint32_t size1)
{
    ddr3_wr(FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 0, 0)), FX1_DDR_BASE);
    ddr3_wr(FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 0, 1)), size0);
    ddr3_wr(FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 1, 0)), base1);
    ddr3_wr(FX1_DDR3_FE_REGION_SHADOW(FX1_DDR3_REGION_INDEX(0, 1, 1)), size1);
    ddr3_wr(FX1_DDR3_FE_REGION_COMMIT, 1); /* a rejection faults the store (default handler) */
    fx1_check(ddr3_rd(FX1_DDR3_FE_REGION_STATUS) & FX1_DDR3_FE_REGION_ST_VALID, 0x60, "region image installed");
}

void secondary_main(uint32_t hart)
{
    (void)hart;
    while (!go) __asm__ volatile("nop");
    fx1_acquire();
    at_write(NAME_B(0), 0x22222222u);
    fx1_release();
    done = 1;
    for (;;) fx1_wfi();
}

int main(void)
{
    fx1_log("DDR3 region aliases and the exclusive monitor");
    commit_regions(0x18000000u, 0x98000000u, 0x08000000u);

    /* A */
    at_write(NAME_A(0x100), 0x5A5A1234u);
    fx1_check(at_read(NAME_B(0x100)) == 0x5A5A1234u, 0x61, "A: the two names share storage");
    fx1_log("A: aliases share data");

    /* B */
    at_write(NAME_A(0), 0x11111111u);
    fx1_release_secondaries();
    const uint32_t old = at_lr(NAME_A(0));
    fx1_release();
    go = 1;
    while (!done) __asm__ volatile("nop");
    fx1_acquire();
    fx1_check(old == 0x11111111u, 0x62, "B: LR value");
    fx1_check(at_sc(NAME_A(0), 0x33333333u) != 0, 0x63, "B: a store through the alias voids the reservation");
    fx1_check(at_read(NAME_A(0)) == 0x22222222u, 0x64, "B: hart 1's data kept");
    fx1_log("B: hart store through the alias -> SC fails");

    /* C */
    at_write(AT_SRC, 0xD0D0D0D0u);
    at_write(NAME_A(0x40), 0x44444444u);
    (void)at_lr(NAME_A(0x40));
    at_dma_start(AT_SRC, NAME_B(0x40), 4, 0);
    while (!at_dma_done()) {
    }
    at_dma_ack();
    fx1_dma_complete((const volatile void*)NAME_B(0x40), 4, FX1_DMA_FROM_DEVICE);
    fx1_check(at_sc(NAME_A(0x40), 0x55555555u) != 0, 0x65, "C: a DMA write through the alias voids the reservation");
    fx1_check(at_read(NAME_A(0x40)) == 0xD0D0D0D0u, 0x66, "C: the DMA data kept");
    fx1_log("C: DMA write through the alias -> SC fails");

    /* D */
    at_write(AT_SRC, DMA_VALUE);
    uint32_t amos = 0;
    for (uint32_t t = 0; t < TRIALS; ++t) {
        const uint32_t off = 0x1000u + t * AT_GRANULE;
        at_write(NAME_A(off), 0);
        at_dma_start(AT_SRC, NAME_B(off), 4, AT_PAD_BYTES - (t * 37u) % 700u);
        uint32_t k = 0;
        do {
            for (uint32_t i = 0; i < 8u; ++i, ++k) (void)at_amoor(NAME_A(off), 1u << (k % 31u));
        } while (!at_dma_done());
        at_dma_ack();
        fx1_dma_complete((const volatile void*)NAME_B(off), 4, FX1_DMA_FROM_DEVICE);
        amos += k;
        const uint32_t final = at_read(NAME_A(off));
        if (!(final & DMA_VALUE)) {
            fx1_log_hex("trial", t);
            fx1_log_hex("  final value (bit 31 = the DMA write)", final);
            fx1_fail(0x67);
        }
    }
    fx1_log_hex("D: amoor.w executed", amos);
    fx1_log("D: no DMA write through the alias lost inside an AMO");

    commit_regions(FX1_DDR_SIZE, 0, 0);
    return 0;
}
