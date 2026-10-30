// Host check for gbeink.c: runs a ROM, applies every step()'s window to two
// simulated RAM planes, and checks after each step that 0x24 holds the current
// frame everywhere and 0x26 equals 0x24 outside the window (nothing stray is
// driven). Prints window stats and writes the last panel image as a PBM.
#include <stdio.h>
#include <stdlib.h>
#include "gbeink.c"

#define PW 50
#define PH 300

static uint8_t r24[PW * PH], r26[PW * PH];

// The current frame as panel bytes, for the check.
static void expand(ctx_t *ctx, const uint8_t *fr, uint8_t *dst) {
    memset(dst, 0xFF, PW * PH);
    for (int l = 0; l < GB_H; l++) {
        for (int c = 0; c < LINE_BYTES; c++) {
            uint8_t b = fr[l * LINE_BYTES + c];
            dst[(ctx->y0 + 2 * l) * PW + ctx->x0 + c] = ctx->top[b];
            dst[(ctx->y0 + 2 * l + 1) * PW + ctx->x0 + c] = ctx->bot[b];
        }
    }
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s rom.gb [frames] [lag] [out.pbm]\n", argv[0]); return 2; }
    uint32_t frames = argc > 2 ? atoi(argv[2]) : 3000;
    uint32_t lag = argc > 3 ? atoi(argv[3]) : 6;
    FILE *fp = fopen(argv[1], "rb");
    static uint8_t rom[1 << 20];
    size_t n = fread(rom, 1, sizeof rom, fp); fclose(fp);
    const uint8_t *banks[MAX_BANKS];
    for (size_t i = 0; i < n >> BANK_SHIFT; i++) banks[i] = rom + (i << BANK_SHIFT);
    static ctx_t ctx;
    int e = pgb_init(&ctx, banks, n >> BANK_SHIFT);
    if (e) { printf("init error %d\n", e); return 1; }
    for (uint32_t i = 0; i <= lag; i++) ctx.ring[i] = calloc(1, FRAME_SIZE);
    ctx.slots = lag + 1; ctx.x0 = 5; ctx.y0 = 6;
    pgb_reset(&ctx);
    memset(r24, 0xFF, sizeof r24); memset(r26, 0xFF, sizeof r26);
    static uint8_t out[2 * PW * PH], want[PW * PH];
    uint64_t total = 0; uint32_t steps_on = 0, maxsz = 0, bad = 0;
    for (uint32_t f = 0; f < frames; f++) {
        pgb_run_frames(&ctx, 1);
        if (ctx.err) { printf("core error %u at %04x\n", ctx.err, ctx.err_addr); return 1; }
        uint32_t cur_slot = ctx.head;
        expand(&ctx, ctx.ring[cur_slot], want);
        uint32_t box[4];
        uint32_t sz = 0;
        if (pgb_step(&ctx, box)) {
            // stream through a small buffer, as on the board
            static uint8_t chunk[2304];
            uint32_t k;
            while ((k = pgb_plane(&ctx, chunk, sizeof chunk, 0)) != 0) {
                memcpy(out + sz, chunk, k); sz += k;
            }
            uint32_t sz2 = 0;
            while ((k = pgb_plane(&ctx, chunk, sizeof chunk, 1)) != 0) {
                memcpy(out + sz + sz2, chunk, k); sz2 += k;
            }
            if (sz2 != sz) { printf("plane sizes differ\n"); return 1; }
            uint32_t i = 0;
            for (uint32_t y = box[2]; y <= box[3]; y++)
                for (uint32_t x = box[0]; x <= box[1]; x++, i++) {
                    r26[y * PW + x] = out[i];
                    r24[y * PW + x] = out[sz + i];
                }
            total += sz; steps_on++; if (sz > maxsz) maxsz = sz;
        }
        int ok = memcmp(r24, want, sizeof r24) == 0;
        for (uint32_t y = 0; y < PH && ok; y++)
            for (uint32_t x = 0; x < PW; x++) {
                int in = sz && x >= box[0] && x <= box[1] && y >= box[2] && y <= box[3];
                if (!in && r24[y * PW + x] != r26[y * PW + x]) { ok = 0; break; }
            }
        if (!ok && bad++ < 5) printf("step %u: invariant broken\n", f);
    }
    printf("%u steps, %u with a window, avg %.0f B/plane, max %u B/plane (full 15000), %u bad\n",
           frames, steps_on, steps_on ? (double)total / steps_on : 0.0, maxsz, bad);
    if (argc > 4) {
        FILE *o = fopen(argv[4], "wb");
        fprintf(o, "P4\n400 300\n");
        for (int i = 0; i < PW * PH; i++) fputc((uint8_t)~r24[i], o);
        fclose(o);
    }
    return bad != 0;
}
