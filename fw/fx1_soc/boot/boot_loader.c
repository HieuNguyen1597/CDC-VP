/*
 * FX1 BootROM loader for the firmware-init boot (DDR3 VP plan §5.2, subtask 5).
 * Linked with fx1_rom.ld: runs from BootROM with data and stacks in SRAM.
 *
 *  1. Hart 0 brings the DDR3 controller up (ddr3_bringup: identity, QoS sideband,
 *     timing/DFI/INIT/topology images, commits, GLOBAL_CTRL.enable) and waits for
 *     mission readiness, startup ECC scrub included. Hart 1 waits in WFI (crt0_rom.S)
 *     and touches no DDR. A failure or a readiness timeout is reported on the UART
 *     and through sim-control, from BootROM/SRAM only.
 *  2. Hart 0 copies the PT_LOAD segments of the application ELF embedded in BootROM
 *     (.app_image) into DDR. The application zeroes its own .bss (crt0.S).
 *  3. Hart 0 releases hart 1; both harts jump to the application entry, which then
 *     runs its usual start-up (hart 1 parks until the application releases it).
 *
 * Failure codes (sim-control FAIL): 0xD300 | ddr3_status_t for the bring-up,
 * 0xD310.. for an invalid application image.
 */
#include "ddr3_fw.h"

#ifndef BOOT_READY_TIMEOUT_MS
#define BOOT_READY_TIMEOUT_MS 2000u /* the 4 GiB ECC scrub takes about 670 ms */
#endif

extern const uint8_t fx1_app_image[], fx1_app_image_end[];

static volatile uint32_t boot_entry;
static volatile uint32_t hart1_started;

static uint32_t rd16(const uint8_t* p) { return (uint32_t)p[0] | (uint32_t)p[1] << 8; }
static uint32_t rd32(const uint8_t* p) { return rd16(p) | rd16(p + 2) << 16; }

static void __attribute__((noreturn)) bad_image(uint32_t code, const char* what)
{
    fx1_log(what);
    fx1_fail(code);
}

/* Copies every PT_LOAD segment into DDR; returns the entry point. */
static uint32_t load_application(void)
{
    const uint8_t* elf = fx1_app_image;
    const uint32_t size = (uint32_t)(fx1_app_image_end - fx1_app_image);
    if (size < 52 || rd32(elf) != 0x464C457Fu) bad_image(0xD310, "boot: no ELF image in BootROM");
    if (elf[4] != 1 || elf[5] != 1 || rd16(elf + 18) != 243u)
        bad_image(0xD311, "boot: not a little-endian RV32 ELF");
    const uint32_t entry = rd32(elf + 24), phoff = rd32(elf + 28);
    const uint32_t phentsize = rd16(elf + 42), phnum = rd16(elf + 44);
    if (phentsize != 32u || phoff > size || phnum > (size - phoff) / 32u)
        bad_image(0xD312, "boot: bad program header table");
    for (uint32_t i = 0; i < phnum; ++i) {
        const uint8_t* ph = elf + phoff + 32u * i;
        if (rd32(ph) != 1u) continue; /* PT_LOAD */
        const uint32_t offset = rd32(ph + 4), paddr = rd32(ph + 12), filesz = rd32(ph + 16);
        if (filesz == 0) continue;
        if (offset > size || filesz > size - offset) bad_image(0xD313, "boot: segment outside the image");
        if (paddr < FX1_DDR_BASE || filesz > FX1_DDR_SIZE || paddr - FX1_DDR_BASE > FX1_DDR_SIZE - filesz)
            bad_image(0xD314, "boot: segment outside DDR");
        const uint8_t* src = elf + offset;
        if (((uintptr_t)src | paddr | filesz) % 4u == 0) {
            for (uint32_t k = 0; k < filesz; k += 4) *(volatile uint32_t*)(paddr + k) = rd32(src + k);
        } else {
            for (uint32_t k = 0; k < filesz; ++k) *(volatile uint8_t*)(paddr + k) = src[k];
        }
        fx1_log_hex("boot: loaded segment at", paddr);
        fx1_log_hex("  bytes", filesz);
    }
    if (entry < FX1_DDR_BASE || entry - FX1_DDR_BASE >= FX1_DDR_SIZE) bad_image(0xD315, "boot: entry outside DDR");
    return entry;
}

static void __attribute__((noreturn)) start_application(uint32_t entry)
{
    /* The image was written by stores: order them before instruction fetches. */
    __asm__ volatile("fence rw, rw\n\tfence.i\n\tjr %0" ::"r"(entry) : "memory");
    __builtin_unreachable();
}

int main(void)
{
    ddr3_install_probe_handler();
    fx1_log("boot: FX1 BootROM loader (DDR3 firmware init)");
    const ddr3_status_t status = ddr3_bringup(BOOT_READY_TIMEOUT_MS);
    if (status != DDR3_OK) {
        ddr3_log_status("boot: DDR3 bring-up failed:", status);
        fx1_fail(0xD300u | (uint32_t)status);
    }
    fx1_log_hex("boot: DDR3 ready, waited us", ddr3_last.elapsed_us);
    const uint32_t entry = load_application();
    fx1_log_hex("boot: starting the application at", entry);
    boot_entry = entry;
    fx1_release_secondaries();
    /* Hart 1 must have acknowledged its release IPI before the application can send
     * its own (crt0.S parks hart 1 again until then). */
    while (!hart1_started) ddr3_sleep_us(1u);
    start_application(entry);
}

void secondary_main(uint32_t hart)
{
    (void)hart;
    const uint32_t entry = boot_entry;
    fx1_mb();
    hart1_started = 1;
    start_application(entry);
}
