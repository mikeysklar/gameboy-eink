// Host build of gbeink.c as a shared library, so CPython (ctypes) can drive the
// same core and run board-side Python like bot.py against it.
#include <stdlib.h>
#include "gbeink.c"

static ctx_t ctx;
static uint8_t *rom_buf;

int lib_init(const uint8_t *rom, uint32_t len) {
    rom_buf = malloc(len);
    memcpy(rom_buf, rom, len);
    const uint8_t *banks[MAX_BANKS];
    for (uint32_t i = 0; i < len >> BANK_SHIFT; i++) banks[i] = rom_buf + (i << BANK_SHIFT);
    return pgb_init(&ctx, banks, len >> BANK_SHIFT);
}
void lib_set_ring(uint32_t slots) {
    for (uint32_t i = 0; i < slots; i++) ctx.ring[i] = calloc(1, FRAME_SIZE);
    ctx.slots = slots; ctx.x0 = 5; ctx.y0 = 6;
    pgb_reset(&ctx);
}
uint32_t lib_run(uint32_t n) { return pgb_run_frames(&ctx, n); }
void lib_joypad(uint32_t m) { pgb_set_joypad(&ctx, m); }
void lib_read(uint32_t addr, uint8_t *buf, uint32_t n) { pgb_read(&ctx, addr, buf, n); }
const uint8_t *lib_frame(void) { return ctx.ring[ctx.head]; }
