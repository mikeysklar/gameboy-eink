// SPDX-FileCopyrightText: 2026 Mikey Sklar for Adafruit Industries
// SPDX-License-Identifier: MIT
//
// Native module `gbeink`: Game Boy (DMG only, no sound) for a 400x300 SSD1683
// e-paper panel driven with the balanced lag trick from eink-high-freq.
// Peanut-GB (deltabeard, MIT) in tvecera's fork, the same core file as
// fruitjam-arcade/ports/gbc, built with PEANUT_FULL_GBC_SUPPORT 0.
//
// Every buffer lives in a Python-owned bytearray and is passed in, so the GC
// sees all of it and nothing has to be allocated from native code:
//
//   st = bytearray(gbeink.STATE_SIZE)            # core + context, keep alive
//   gbeink.init(st, rom)                         # rom: list of 16 KiB banks, keep alive
//   ring = [bytearray(gbeink.FRAME_SIZE) for _ in range(lag + 1)]
//   gbeink.set_ring(st, ring, x0, y0)            # x0 in RAM bytes, y0 in rows
//   gbeink.set_joypad(st, mask)                  # 1 = pressed, see JOYPAD_* below
//   gbeink.run_frames(st, n)                     # -> frames run, the last one is kept
//   gbeink.blank(st)                             # current frame all white instead
//   box = gbeink.step(st)                        # see below
//   sz = gbeink.plane(st, out, 0)                # next out[:sz] for RAM 0x26
//   sz = gbeink.plane(st, out, 1)                # next out[:sz] for RAM 0x24
//   gbeink.reset(st)                             # every frame white, after a wipe
//   gbeink.read(st, addr, buf)                   # buf <- Game Boy memory at addr
//
// The ROM and the ring are lists of small buffers so the RP2040's heap never
// needs one 32-40 KB block. The ring holds the last lag + 1 frames as 2-bit
// shades, 40 bytes per line.
// step() compares the current frame with the one lag steps back and returns
// None when nothing needs driving, else (x0, x1, y0, y1) in panel RAM bytes and
// rows. The box covers the pixels that differ now and those that differed last
// step, so the two RAM planes stay equal outside it. Then the ring moves on one
// frame. plane() fills out with the next whole lines of the box from the old
// frame (0, for RAM 0x26) or the current one (1, for RAM 0x24) and returns the
// length, then 0 once the box is done, so a small out can stream a big box.
// Call it before the next run_frames(), which draws over the old frame's slot.
//
// Each Game Boy pixel becomes 2x2 panel pixels: white, 1 of 4 black, 3 of 4
// black, black. The patterns are fixed, so a still picture never flips.

#ifdef HOST_TEST
#include <stdint.h>
#include <stddef.h>
#include <string.h>
#else
#include "py/dynruntime.h"

// dynruntime.h does not map these; Peanut-GB and GCC's struct copies need them.
void *memset(void *s, int c, size_t n) {
    return mp_fun_table.memset_(s, c, n);
}
void *memmove(void *dest, const void *src, size_t n) {
    return mp_fun_table.memmove_(dest, src, n);
}
void *memcpy(void *dest, const void *src, size_t n) {
    return mp_fun_table.memmove_(dest, src, n);
}
#endif

#define ENABLE_SOUND 0
#define ENABLE_LCD 1

#include <stdbool.h>
#define PEANUT_FULL_GBC_SUPPORT 0
// The core marks the code after its error callback unreachable, but our
// callback returns (see gb_error_cb). Make that path defined behaviour.
#define PGB_UNREACHABLE() ((void)0)

// The core sorts a line's sprites (at most 10 of 40) with qsort(), which a
// native module cannot link. Stable insertion sort, same result for the
// core's comparator (X, then OAM index).
static void pgb_qsort(void *base, size_t n, size_t size, int (*cmp)(const void *, const void *)) {
    uint8_t *a = base;
    uint8_t tmp[8];  // sprite_data is 2 bytes
    for (size_t i = 1; i < n; i++) {
        size_t j = i;
        memcpy(tmp, a + i * size, size);
        while (j > 0 && cmp(a + (j - 1) * size, tmp) > 0) {
            memcpy(a + j * size, a + (j - 1) * size, size);
            j--;
        }
        memcpy(a + j * size, tmp, size);
    }
}
#define qsort pgb_qsort

// Upstream Peanut-GB's joypad bits; the fork leaves them to the frontend.
#define JOYPAD_A 0x01
#define JOYPAD_B 0x02
#define JOYPAD_SELECT 0x04
#define JOYPAD_START 0x08
#define JOYPAD_RIGHT 0x10
#define JOYPAD_LEFT 0x20
#define JOYPAD_UP 0x40
#define JOYPAD_DOWN 0x80

// The fork trips warnings that the natmod build treats as errors.
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wsequence-point"
#pragma GCC diagnostic ignored "-Wmisleading-indentation"
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"
#pragma GCC diagnostic ignored "-Wunused-variable"
#pragma GCC diagnostic ignored "-Wunused-function"
#include "peanut_gb_cgb.h"
#pragma GCC diagnostic pop

#define GB_W 160
#define GB_H 144
#define LINE_BYTES (GB_W / 4)
#define FRAME_SIZE (LINE_BYTES * GB_H)
#define BANK_SHIFT 14
#define MAX_BANKS 64        // 1 MiB of ROM
#define MAX_SLOTS 16        // lag up to 15

typedef struct {
    struct gb_s gb;
    const uint8_t *rom[MAX_BANKS];
    uint32_t banks;
    uint32_t err;
    uint32_t err_addr;
    uint8_t *ring[MAX_SLOTS];
    uint32_t slots;     // lag + 1, 0 until set_ring()
    uint32_t head;      // slot of the current frame
    uint32_t x0, y0;    // panel RAM byte column and row of the picture's corner
    // last step's changed area in ring bytes and lines; c0 > c1 when empty
    int32_t pc0, pc1, pl0, pl1;
    // the last step's box in ring bytes and lines, and the two frames it compared
    int32_t bc0, bc1, bl0, bl1;
    uint32_t cur_slot, old_slot;
    int32_t next_line;  // plane()'s position in the box, -1 between planes
    uint8_t top[256];   // 4 packed shades -> panel byte for the upper row
    uint8_t bot[256];   // and the lower row
} ctx_t;

static uint8_t rom_read(struct gb_s *gb, const uint_fast32_t addr) {
    ctx_t *ctx = gb->direct.priv;
    const uint32_t bank = addr >> BANK_SHIFT;
    return bank < ctx->banks ? ctx->rom[bank][addr & ((1u << BANK_SHIFT) - 1)] : 0xFF;
}

// No cartridge RAM: e-paper demos do not save.
static uint8_t cart_ram_read(struct gb_s *gb, const uint_fast32_t addr) {
    return 0xFF;
}

static void cart_ram_write(struct gb_s *gb, const uint_fast32_t addr, const uint8_t val) {
}

static void gb_error_cb(struct gb_s *gb, const enum gb_error_e err, const uint16_t addr) {
    ctx_t *ctx = gb->direct.priv;
    // Peanut-GB keeps running after calling this. End the frame now so
    // gb_run_frame() returns after this instruction; run_frames() then stops.
    ctx->err = (uint32_t)err + 1;
    ctx->err_addr = addr;
    gb->gb_frame = 1;
}

// Low two bits of each pixel are the palette-mapped shade, 0 lightest. Packed
// four to a byte, leftmost pixel in the top bits like the panel.
static void draw_line(struct gb_s *gb, const uint8_t *pixels, const uint_fast8_t line) {
    ctx_t *ctx = gb->direct.priv;
    if (ctx->slots == 0) {
        return;
    }
    uint8_t *dst = ctx->ring[ctx->head] + line * LINE_BYTES;
    for (unsigned x = 0; x < GB_W; x += 4) {
        *dst++ = (uint8_t)(((pixels[x] & 3) << 6) | ((pixels[x + 1] & 3) << 4) |
            ((pixels[x + 2] & 3) << 2) | (pixels[x + 3] & 3));
    }
}

// 2x2 cell per shade, panel bit 1 = white. Upper row (left, right), lower row.
static void build_tables(ctx_t *ctx) {
    static const uint8_t up[4] = {3, 1, 1, 0};
    static const uint8_t lo[4] = {3, 3, 0, 0};
    for (unsigned b = 0; b < 256; b++) {
        uint8_t t = 0, l = 0;
        for (unsigned k = 0; k < 4; k++) {
            unsigned s = (b >> (6 - 2 * k)) & 3;
            t = (uint8_t)((t << 2) | up[s]);
            l = (uint8_t)((l << 2) | lo[s]);
        }
        ctx->top[b] = t;
        ctx->bot[b] = l;
    }
}

static void pgb_reset(ctx_t *ctx) {
    for (uint32_t i = 0; i < ctx->slots; i++) {
        memset(ctx->ring[i], 0, FRAME_SIZE);
    }
    ctx->head = 0;
    ctx->pc0 = LINE_BYTES;
    ctx->pc1 = -1;
    ctx->pl0 = GB_H;
    ctx->pl1 = -1;
    ctx->bc0 = 0;
    ctx->bc1 = -1;
    ctx->next_line = -1;
}

// rom: banks of 16 KiB each, set by the caller after the memset.
static int pgb_init(ctx_t *ctx, const uint8_t *const *rom, uint32_t banks) {
    memset(ctx, 0, sizeof(*ctx));
    for (uint32_t i = 0; i < banks; i++) {
        ctx->rom[i] = rom[i];
    }
    ctx->banks = banks;
    build_tables(ctx);
    pgb_reset(ctx);
    enum gb_init_error_e e = gb_init(&ctx->gb, rom_read, cart_ram_read, cart_ram_write, gb_error_cb, ctx);
    if (e != GB_INIT_NO_ERROR) {
        return (int)e;
    }
    gb_init_lcd(&ctx->gb, draw_line);
    return 0;
}

static uint32_t pgb_run_frames(ctx_t *ctx, uint32_t n) {
    uint32_t i = 0;
    while (i < n && ctx->err == 0) {
        gb_run_frame(&ctx->gb);
        i++;
    }
    return i;
}

// Box of this step's changes plus last step's in panel RAM bytes and rows
// (x0, x1, y0, y1). Returns 0 when there is nothing to drive.
static uint32_t pgb_step(ctx_t *ctx, uint32_t box[4]) {
    const uint32_t old_slot = (ctx->head + 1) % ctx->slots;
    const uint8_t *cur = ctx->ring[ctx->head];
    const uint8_t *old = ctx->ring[old_slot];
    int32_t c0 = LINE_BYTES, c1 = -1, l0 = GB_H, l1 = -1;
    for (int32_t l = 0; l < GB_H; l++) {
        const uint8_t *a = cur + l * LINE_BYTES;
        const uint8_t *b = old + l * LINE_BYTES;
        int32_t first = -1, last = -1;
        for (int32_t c = 0; c < LINE_BYTES; c++) {
            if (a[c] != b[c]) {
                if (first < 0) {
                    first = c;
                }
                last = c;
            }
        }
        if (first >= 0) {
            if (l0 > l) {
                l0 = l;
            }
            l1 = l;
            if (c0 > first) {
                c0 = first;
            }
            if (c1 < last) {
                c1 = last;
            }
        }
    }
    // union with the previous step's changes, then remember this step's
    int32_t u0 = c0 < ctx->pc0 ? c0 : ctx->pc0;
    int32_t u1 = c1 > ctx->pc1 ? c1 : ctx->pc1;
    int32_t v0 = l0 < ctx->pl0 ? l0 : ctx->pl0;
    int32_t v1 = l1 > ctx->pl1 ? l1 : ctx->pl1;
    ctx->pc0 = c0;
    ctx->pc1 = c1;
    ctx->pl0 = l0;
    ctx->pl1 = l1;
    ctx->cur_slot = ctx->head;
    ctx->old_slot = old_slot;
    ctx->head = old_slot;  // the oldest slot becomes the next frame's
    ctx->bc0 = u0;
    ctx->bc1 = u1;
    ctx->bl0 = v0;
    ctx->bl1 = v1;
    ctx->next_line = -1;
    if (u0 > u1 || v0 > v1) {
        ctx->bc1 = -1;
        return 0;
    }
    box[0] = ctx->x0 + u0;
    box[1] = ctx->x0 + u1;
    box[2] = ctx->y0 + 2 * v0;
    box[3] = ctx->y0 + 2 * v1 + 1;
    return 1;
}

// The next lines of the last box from the old (which 0) or current (1) frame
// as panel bytes. Returns bytes written, 0 when the box is done (or empty),
// 0xFFFFFFFF if out cannot hold one line.
static uint32_t pgb_plane(ctx_t *ctx, uint8_t *out, uint32_t out_len, uint32_t which) {
    if (ctx->bc0 > ctx->bc1) {
        return 0;
    }
    if (ctx->next_line < 0) {
        ctx->next_line = ctx->bl0;
    } else if (ctx->next_line > ctx->bl1) {
        ctx->next_line = -1;
        return 0;
    }
    const uint32_t w = (uint32_t)(ctx->bc1 - ctx->bc0 + 1);
    uint32_t lines = out_len / (2 * w);
    if (lines == 0) {
        return 0xFFFFFFFF;
    }
    if (lines > (uint32_t)(ctx->bl1 - ctx->next_line + 1)) {
        lines = (uint32_t)(ctx->bl1 - ctx->next_line + 1);
    }
    const uint8_t *src = ctx->ring[which ? ctx->cur_slot : ctx->old_slot] +
        ctx->next_line * LINE_BYTES + ctx->bc0;
    for (uint32_t l = 0; l < lines; l++) {
        for (uint32_t c = 0; c < w; c++) {
            out[c] = ctx->top[src[c]];
            out[c + w] = ctx->bot[src[c]];
        }
        src += LINE_BYTES;
        out += 2 * w;
    }
    ctx->next_line += (int32_t)lines;
    return lines * 2 * w;
}

// Copy n bytes of the Game Boy's memory map (VRAM, OAM, WRAM, HRAM...) to buf,
// for a frontend that watches game state.
static void pgb_read(ctx_t *ctx, uint32_t addr, uint8_t *buf, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        buf[i] = __gb_read(&ctx->gb, (uint16_t)(addr + i));
    }
}

// Joypad mask uses 1 = pressed; Peanut-GB's register is active low.
static void pgb_set_joypad(ctx_t *ctx, uint32_t mask) {
    ctx->gb.direct.joypad = (uint8_t)~mask;
}

#ifndef HOST_TEST

static ctx_t *get_ctx(mp_obj_t st_in) {
    mp_buffer_info_t buf;
    mp_get_buffer_raise(st_in, &buf, MP_BUFFER_RW);
    if (buf.len < sizeof(ctx_t)) {
        mp_raise_ValueError(MP_ERROR_TEXT("state buffer too small"));
    }
    return (ctx_t *)buf.buf;
}

static mp_obj_t mod_init(mp_obj_t st_in, mp_obj_t rom_in) {
    ctx_t *ctx = get_ctx(st_in);
    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(rom_in, &n, &items);
    if (n == 0 || n > MAX_BANKS) {
        mp_raise_ValueError(MP_ERROR_TEXT("bad bank count"));
    }
    const uint8_t *banks[MAX_BANKS];
    for (size_t i = 0; i < n; i++) {
        mp_buffer_info_t b;
        mp_get_buffer_raise(items[i], &b, MP_BUFFER_READ);
        if (b.len != (1u << BANK_SHIFT)) {
            mp_raise_ValueError(MP_ERROR_TEXT("banks must be 16 KiB"));
        }
        banks[i] = b.buf;
    }
    return mp_obj_new_int(pgb_init(ctx, banks, n));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_init_obj, mod_init);

// set_ring(st, ring, x0, y0): ring is a list of lag + 1 frame buffers
static mp_obj_t mod_set_ring(size_t n_args, const mp_obj_t *args) {
    ctx_t *ctx = get_ctx(args[0]);
    size_t n;
    mp_obj_t *items;
    mp_obj_get_array(args[1], &n, &items);
    if (n < 2 || n > MAX_SLOTS) {
        mp_raise_ValueError(MP_ERROR_TEXT("ring needs 2 to 16 frames"));
    }
    for (size_t i = 0; i < n; i++) {
        mp_buffer_info_t b;
        mp_get_buffer_raise(items[i], &b, MP_BUFFER_WRITE);
        if (b.len < FRAME_SIZE) {
            mp_raise_ValueError(MP_ERROR_TEXT("ring frame too small"));
        }
        ctx->ring[i] = b.buf;
    }
    ctx->slots = n;
    ctx->x0 = mp_obj_get_int(args[2]);
    ctx->y0 = mp_obj_get_int(args[3]);
    pgb_reset(ctx);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_VAR_BETWEEN(mod_set_ring_obj, 4, 4, mod_set_ring);

static mp_obj_t mod_set_joypad(mp_obj_t st_in, mp_obj_t mask_in) {
    pgb_set_joypad(get_ctx(st_in), mp_obj_get_int(mask_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_set_joypad_obj, mod_set_joypad);

static mp_obj_t mod_run_frames(mp_obj_t st_in, mp_obj_t n_in) {
    return mp_obj_new_int(pgb_run_frames(get_ctx(st_in), mp_obj_get_int(n_in)));
}
static MP_DEFINE_CONST_FUN_OBJ_2(mod_run_frames_obj, mod_run_frames);

static mp_obj_t mod_blank(mp_obj_t st_in) {
    ctx_t *ctx = get_ctx(st_in);
    if (ctx->slots != 0) {
        memset(ctx->ring[ctx->head], 0, FRAME_SIZE);
    }
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_blank_obj, mod_blank);

static mp_obj_t mod_reset(mp_obj_t st_in) {
    pgb_reset(get_ctx(st_in));
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_reset_obj, mod_reset);

static mp_obj_t mod_step(mp_obj_t st_in) {
    ctx_t *ctx = get_ctx(st_in);
    if (ctx->slots == 0) {
        mp_raise_ValueError(MP_ERROR_TEXT("no ring"));
    }
    uint32_t box[4];
    if (!pgb_step(ctx, box)) {
        return mp_const_none;
    }
    mp_obj_t t[4] = {
        MP_OBJ_NEW_SMALL_INT(box[0]), MP_OBJ_NEW_SMALL_INT(box[1]),
        MP_OBJ_NEW_SMALL_INT(box[2]), MP_OBJ_NEW_SMALL_INT(box[3]),
    };
    return mp_obj_new_tuple(4, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_step_obj, mod_step);

static mp_obj_t mod_plane(mp_obj_t st_in, mp_obj_t out_in, mp_obj_t which_in) {
    ctx_t *ctx = get_ctx(st_in);
    mp_buffer_info_t out;
    mp_get_buffer_raise(out_in, &out, MP_BUFFER_WRITE);
    uint32_t sz = pgb_plane(ctx, out.buf, out.len, mp_obj_get_int(which_in));
    if (sz == 0xFFFFFFFF) {
        mp_raise_ValueError(MP_ERROR_TEXT("out too small"));
    }
    return MP_OBJ_NEW_SMALL_INT(sz);
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_plane_obj, mod_plane);

static mp_obj_t mod_read(mp_obj_t st_in, mp_obj_t addr_in, mp_obj_t buf_in) {
    ctx_t *ctx = get_ctx(st_in);
    mp_buffer_info_t buf;
    mp_get_buffer_raise(buf_in, &buf, MP_BUFFER_WRITE);
    pgb_read(ctx, mp_obj_get_int(addr_in), buf.buf, buf.len);
    return mp_const_none;
}
static MP_DEFINE_CONST_FUN_OBJ_3(mod_read_obj, mod_read);

// error(st) -> (code, addr); code 0 means no error, else gb_error_e + 1.
static mp_obj_t mod_error(mp_obj_t st_in) {
    ctx_t *ctx = get_ctx(st_in);
    mp_obj_t t[2] = {mp_obj_new_int(ctx->err), mp_obj_new_int(ctx->err_addr)};
    return mp_obj_new_tuple(2, t);
}
static MP_DEFINE_CONST_FUN_OBJ_1(mod_error_obj, mod_error);

mp_obj_t mpy_init(mp_obj_fun_bc_t *self, size_t n_args, size_t n_kw, mp_obj_t *args) {
    MP_DYNRUNTIME_INIT_ENTRY
    mp_store_global(MP_QSTR_STATE_SIZE, MP_OBJ_NEW_SMALL_INT(sizeof(ctx_t)));
    mp_store_global(MP_QSTR_FRAME_SIZE, MP_OBJ_NEW_SMALL_INT(FRAME_SIZE));
    mp_store_global(MP_QSTR_JOYPAD_A, MP_OBJ_NEW_SMALL_INT(JOYPAD_A));
    mp_store_global(MP_QSTR_JOYPAD_B, MP_OBJ_NEW_SMALL_INT(JOYPAD_B));
    mp_store_global(MP_QSTR_JOYPAD_SELECT, MP_OBJ_NEW_SMALL_INT(JOYPAD_SELECT));
    mp_store_global(MP_QSTR_JOYPAD_START, MP_OBJ_NEW_SMALL_INT(JOYPAD_START));
    mp_store_global(MP_QSTR_JOYPAD_RIGHT, MP_OBJ_NEW_SMALL_INT(JOYPAD_RIGHT));
    mp_store_global(MP_QSTR_JOYPAD_LEFT, MP_OBJ_NEW_SMALL_INT(JOYPAD_LEFT));
    mp_store_global(MP_QSTR_JOYPAD_UP, MP_OBJ_NEW_SMALL_INT(JOYPAD_UP));
    mp_store_global(MP_QSTR_JOYPAD_DOWN, MP_OBJ_NEW_SMALL_INT(JOYPAD_DOWN));
    mp_store_global(MP_QSTR_init, MP_OBJ_FROM_PTR(&mod_init_obj));
    mp_store_global(MP_QSTR_set_ring, MP_OBJ_FROM_PTR(&mod_set_ring_obj));
    mp_store_global(MP_QSTR_set_joypad, MP_OBJ_FROM_PTR(&mod_set_joypad_obj));
    mp_store_global(MP_QSTR_run_frames, MP_OBJ_FROM_PTR(&mod_run_frames_obj));
    mp_store_global(MP_QSTR_blank, MP_OBJ_FROM_PTR(&mod_blank_obj));
    mp_store_global(MP_QSTR_reset, MP_OBJ_FROM_PTR(&mod_reset_obj));
    mp_store_global(MP_QSTR_step, MP_OBJ_FROM_PTR(&mod_step_obj));
    mp_store_global(MP_QSTR_plane, MP_OBJ_FROM_PTR(&mod_plane_obj));
    mp_store_global(MP_QSTR_read, MP_OBJ_FROM_PTR(&mod_read_obj));
    mp_store_global(MP_QSTR_error, MP_OBJ_FROM_PTR(&mod_error_obj));
    MP_DYNRUNTIME_INIT_EXIT
}

#endif
