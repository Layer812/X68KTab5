/* Build 5.99rc1 - production P4 PIE graphics helpers.
 *
 * Development-only PPA scanline backend, live C/PIE/PPA A/B timing, runtime
 * shadow validation and sample counters were retired after repeated measurements
 * selected PIE by a wide margin.  Production keeps one deterministic startup
 * self-check and exact scalar fallbacks for unsupported alignment/size.
 */
/*
 * Tab5 port-specific implementation.
 * Intent: Compatibility shim retained after the experimental PPA raster staging path was retired; production rendering uses validated PIE/scalar paths.
 * Layer8 Aug/17/2026
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_heap_caps.h"

#define P4BLEND_MAX_WIDTH 800u

static uint8_t s_pie_selfcheck_done;
static uint8_t s_pie_selfcheck_ok;

static inline void scalar_overlay_key0(uint16_t *dst, const uint16_t *top,
                                       uint32_t width)
{
    uint32_t i = 0;
    for (; i + 3u < width; i += 4u) {
        const uint16_t t0 = top[i + 0u];
        const uint16_t t1 = top[i + 1u];
        const uint16_t t2 = top[i + 2u];
        const uint16_t t3 = top[i + 3u];
        if (t0) dst[i + 0u] = t0;
        if (t1) dst[i + 1u] = t1;
        if (t2) dst[i + 2u] = t2;
        if (t3) dst[i + 3u] = t3;
    }
    for (; i < width; ++i) {
        const uint16_t t = top[i];
        if (t) dst[i] = t;
    }
}

#if defined(ESP_PLATFORM) && defined(__riscv)
/* Eight RGB565 pixels per iteration.
 * q2 = (top == 0) ? 0xffff : 0 for each U16 lane. Since top itself is
 * already zero in every transparent lane, the exact scalar identity is simply
 *     result = top | (bottom & q2)
 * No NOT and no masking of top are required.  This is both shorter and avoids
 * the 5.98g NOT/mask sequence implicated by the transparent-pixel corruption. */
__asm__(
    ".section .iram1,\"ax\",@progbits\n"
    ".global tab5_pie_key0_overlay8\n"
    ".type tab5_pie_key0_overlay8, @function\n"
    ".balign 4\n"
    "tab5_pie_key0_overlay8:\n"
    "beqz a2, 2f\n"
    /* Keep independent load/store cursors for dst.  VLD.128.IP and
     * VST.128.IP both post-increment their base register; using a0 for both
     * skipped one 16-byte destination block per iteration in 5.98g/g1. */
    "mv a3, a0\n"
    "esp.xorq q7, q7, q7\n"
    "1:\n"
    "esp.vld.128.ip q0, a1, 16\n"
    "esp.vld.128.ip q1, a0, 16\n"
    "esp.vcmp.eq.u16 q2, q0, q7\n"
    "esp.andq q1, q1, q2\n"
    "esp.orq q0, q0, q1\n"
    "esp.vst.128.ip q0, a3, 16\n"
    "addi a2, a2, -1\n"
    "bnez a2, 1b\n"
    "2:\n"
    "ret\n"
    ".size tab5_pie_key0_overlay8, .-tab5_pie_key0_overlay8\n"
    ".previous\n"
);
extern void tab5_pie_key0_overlay8(uint16_t *dst, const uint16_t *top,
                                   uint32_t blocks8);
#else
static void tab5_pie_key0_overlay8(uint16_t *dst, const uint16_t *top,
                                   uint32_t blocks8)
{
    scalar_overlay_key0(dst, top, blocks8 * 8u);
}
#endif

static inline void pie_overlay_key0(uint16_t *dst, const uint16_t *top,
                                    uint32_t width)
{
    /* VLD/VST.128 force-align when CFG mis_ld/mis_st are zero.  Every current
     * hot buffer is 64/128-byte aligned, but keep the helper correct for any
     * future caller instead of relying on that project invariant. */
    if ((((uintptr_t)dst | (uintptr_t)top) & 15u) != 0u) {
        scalar_overlay_key0(dst, top, width);
        return;
    }
    const uint32_t blocks = width >> 3;
    const uint32_t vec = blocks << 3;
    if (blocks)
        tab5_pie_key0_overlay8(dst, top, blocks);
    for (uint32_t i = vec; i < width; ++i) {
        const uint16_t t = top[i];
        if (t) dst[i] = t;
    }
}

static uint16_t synth_bottom(uint32_t i, uint32_t pat)
{
    uint32_t v = (i * 1103u + pat * 7919u + 0x1234u) & 0xffffu;
    return (uint16_t)(v ? v : 0x39e7u);
}

static uint16_t synth_top(uint32_t i, uint32_t pat)
{
    switch (pat) {
    case 0: return 0;                                      /* all transparent */
    case 1: {
        uint16_t v = (uint16_t)((i * 977u + 1u) & 0xffffu);
        return v ? v : 1u;
    }
    case 2: return (i & 1u) ? (uint16_t)(0x8001u + i) : 0;
    default:
        if ((i % 3u) == 0u || (i % 11u) == 0u) return 0;
        {
            uint16_t v = (uint16_t)((i * 3571u + 0x55aau) & 0xffffu);
            return v ? v : 0x1234u;
        }
    }
}

static int pie_synthetic_selfcheck(void)
{
    if (s_pie_selfcheck_done)
        return s_pie_selfcheck_ok;
    s_pie_selfcheck_done = 1;

    enum { WORDS = P4BLEND_MAX_WIDTH * 3u };
    uint16_t *scratch = (uint16_t *)heap_caps_aligned_alloc(
        16, WORDS * sizeof(uint16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scratch) {
        s_pie_selfcheck_ok = 0;
        printf("PX68K_P4BLEND599RC1: PIE key0 self-check scratch allocation FAIL; scalar fallback\n");
        return 0;
    }
    uint16_t *bg = scratch;
    uint16_t *top = scratch + P4BLEND_MAX_WIDTH;
    uint16_t *dst = scratch + P4BLEND_MAX_WIDTH * 2u;
    static const uint16_t widths[] = {8, 16, 31, 256, 512, 768, 800};

    for (uint32_t pat = 0; pat < 4u; ++pat) {
        for (uint32_t wi = 0; wi < sizeof(widths)/sizeof(widths[0]); ++wi) {
            const uint32_t width = widths[wi];
            for (uint32_t i = 0; i < width; ++i) {
                bg[i] = synth_bottom(i, pat);
                top[i] = synth_top(i, pat);
                dst[i] = bg[i];
            }
            pie_overlay_key0(dst, top, width);
            for (uint32_t i = 0; i < width; ++i) {
                const uint16_t exp = top[i] ? top[i] : bg[i];
                if (dst[i] != exp) {
                    printf("PX68K_P4BLEND599RC1: PIE key0 self-check FAIL pat=%lu width=%lu x=%lu; scalar fallback\n",
                           (unsigned long)pat, (unsigned long)width, (unsigned long)i);
                    heap_caps_free(scratch);
                    s_pie_selfcheck_ok = 0;
                    return 0;
                }
            }
        }
    }
    heap_caps_free(scratch);
    s_pie_selfcheck_ok = 1;
    printf("PX68K_P4BLEND599RC1: PIE key0 self-check PASS; fixed production backend=PIE-128 with scalar alignment fallback\n");
    return 1;
}


/* Build 5.98g6: small reusable PIE graphics primitive layer.
 *
 * These are deliberately conservative wrappers around four operations that
 * map cleanly to P4 PIE/XesPV: aligned 128-bit copy, byte/RGB565 fill, and equality
 * diff.  They are useful outside the legacy key-zero compositor and make the
 * "PIE as a graphics accelerator" boundary measurable on real PX68K paths.
 *
 * Every wrapper is exact and safe for arbitrary callers: it uses scalar
 * prefix/tail work where practical and falls back completely when two streams
 * cannot be brought to the same 16-byte boundary. */
static uint8_t s_piegfx_init_done;
static uint8_t s_piegfx_copy_ok;
static uint8_t s_piegfx_fill_ok;
static uint8_t s_piegfx_diff_ok;

#if defined(ESP_PLATFORM) && defined(__riscv)
__asm__(
    ".section .iram1,\"ax\",@progbits\n"
    ".global tab5_piegfx_copy16_blocks\n"
    ".type tab5_piegfx_copy16_blocks, @function\n"
    ".balign 4\n"
    "tab5_piegfx_copy16_blocks:\n"
    "beqz a2, 2f\n"
    "1:\n"
    "esp.vld.128.ip q0, a1, 16\n"
    "esp.vst.128.ip q0, a0, 16\n"
    "addi a2, a2, -1\n"
    "bnez a2, 1b\n"
    "2:\n"
    "ret\n"
    ".size tab5_piegfx_copy16_blocks, .-tab5_piegfx_copy16_blocks\n"
    ".previous\n"
);
extern void tab5_piegfx_copy16_blocks(void *dst, const void *src, uint32_t blocks16);

__asm__(
    ".section .iram1,\"ax\",@progbits\n"
    ".global tab5_piegfx_fill16_blocks\n"
    ".type tab5_piegfx_fill16_blocks, @function\n"
    ".balign 4\n"
    "tab5_piegfx_fill16_blocks:\n"
    "beqz a2, 2f\n"
    "esp.vldbc.16.ip q0, a1, 0\n"
    "1:\n"
    "esp.vst.128.ip q0, a0, 16\n"
    "addi a2, a2, -1\n"
    "bnez a2, 1b\n"
    "2:\n"
    "ret\n"
    ".size tab5_piegfx_fill16_blocks, .-tab5_piegfx_fill16_blocks\n"
    ".previous\n"
);
extern void tab5_piegfx_fill16_blocks(uint16_t *dst, const uint16_t *value, uint32_t blocks8);

__asm__(
    ".section .iram1,\"ax\",@progbits\n"
    ".global tab5_piegfx_fill8_blocks\n"
    ".type tab5_piegfx_fill8_blocks, @function\n"
    ".balign 4\n"
    "tab5_piegfx_fill8_blocks:\n"
    "beqz a2, 2f\n"
    "esp.vldbc.8.ip q0, a1, 0\n"
    "1:\n"
    "esp.vst.128.ip q0, a0, 16\n"
    "addi a2, a2, -1\n"
    "bnez a2, 1b\n"
    "2:\n"
    "ret\n"
    ".size tab5_piegfx_fill8_blocks, .-tab5_piegfx_fill8_blocks\n"
    ".previous\n"
);
extern void tab5_piegfx_fill8_blocks(uint8_t *dst, const uint8_t *value, uint32_t blocks16);

__asm__(
    ".section .iram1,\"ax\",@progbits\n"
    ".global tab5_piegfx_diff16_blocks\n"
    ".type tab5_piegfx_diff16_blocks, @function\n"
    ".balign 4\n"
    "tab5_piegfx_diff16_blocks:\n"
    "addi sp, sp, -16\n"
    "beqz a2, 3f\n"
    "1:\n"
    "esp.vld.128.ip q0, a0, 16\n"
    "esp.vld.128.ip q1, a1, 16\n"
    "esp.xorq q2, q0, q1\n"
    /* GCC 14.2 / ESP-IDF 5.4.2 assembler rejects ESP.MAX.U32.A q2,t0.
       Keep the vector XOR and reduce through one aligned 128-bit store through an a-register cursor plus
       four ordinary 32-bit ORs.  The RISC-V ABI guarantees 16-byte SP alignment. */
    "mv a3, sp\n"
    "esp.vst.128.ip q2, a3, 16\n"
    "lw t0, 0(sp)\n"
    "lw t1, 4(sp)\n"
    "or t0, t0, t1\n"
    "lw t1, 8(sp)\n"
    "or t0, t0, t1\n"
    "lw t1, 12(sp)\n"
    "or t0, t0, t1\n"
    "bnez t0, 2f\n"
    "addi a2, a2, -1\n"
    "bnez a2, 1b\n"
    "3:\n"
    "li a0, 0\n"
    "addi sp, sp, 16\n"
    "ret\n"
    "2:\n"
    "li a0, 1\n"
    "addi sp, sp, 16\n"
    "ret\n"
    ".size tab5_piegfx_diff16_blocks, .-tab5_piegfx_diff16_blocks\n"
    ".previous\n"
);
extern int tab5_piegfx_diff16_blocks(const void *a, const void *b, uint32_t blocks16);
#else
static void tab5_piegfx_copy16_blocks(void *dst, const void *src, uint32_t blocks16)
{ memcpy(dst, src, (size_t)blocks16 * 16u); }
static void tab5_piegfx_fill16_blocks(uint16_t *dst, const uint16_t *value, uint32_t blocks8)
{ for (uint32_t i=0;i<blocks8*8u;++i) dst[i]=*value; }
static void tab5_piegfx_fill8_blocks(uint8_t *dst, const uint8_t *value, uint32_t blocks16)
{ for (uint32_t i=0;i<blocks16*16u;++i) dst[i]=*value; }
static int tab5_piegfx_diff16_blocks(const void *a, const void *b, uint32_t blocks16)
{ return memcmp(a,b,(size_t)blocks16*16u)!=0; }
#endif

void tab5_pie_graphics_copy(void *vdst, const void *vsrc, uint32_t bytes)
{
    uint8_t *dst=(uint8_t *)vdst; const uint8_t *src=(const uint8_t *)vsrc;
    if (!dst || !src || !bytes) return;
    const uintptr_t da=(uintptr_t)dst & 15u, sa=(uintptr_t)src & 15u;
    if (!s_piegfx_copy_ok || da != sa || bytes < 64u) { memcpy(dst,src,bytes); return; }
    uint32_t pre=da ? 16u-(uint32_t)da : 0u; if (pre>bytes) pre=bytes;
    if (pre) { memcpy(dst,src,pre); dst+=pre; src+=pre; bytes-=pre; }
    const uint32_t blocks=bytes>>4, vec=blocks<<4;
    if (blocks) { tab5_piegfx_copy16_blocks(dst,src,blocks); }
    if (vec!=bytes) memcpy(dst+vec,src+vec,bytes-vec);
}

void tab5_pie_graphics_fill16(uint16_t *dst, uint16_t value, uint32_t pixels)
{
    if (!dst || !pixels) return;
    if (!s_piegfx_fill_ok || pixels < 32u) { for(uint32_t i=0;i<pixels;++i) dst[i]=value; return; }
    uintptr_t da=(uintptr_t)dst & 15u;
    uint32_t pre=da ? (uint32_t)((16u-da)>>1) : 0u; if(pre>pixels) pre=pixels;
    for(uint32_t i=0;i<pre;++i) dst[i]=value;
    dst+=pre; pixels-=pre;
    const uint32_t blocks=pixels>>3, vec=blocks<<3;
    if (blocks) { tab5_piegfx_fill16_blocks(dst,&value,blocks); }
    for(uint32_t i=vec;i<pixels;++i) dst[i]=value;
}

void tab5_pie_graphics_fill8(uint8_t *dst, uint8_t value, uint32_t bytes)
{
    if (!dst || !bytes) return;
    if (!s_piegfx_fill_ok || bytes < 64u) {
        memset(dst, value, bytes); return;
    }
    uintptr_t da=(uintptr_t)dst & 15u;
    uint32_t pre=da ? 16u-(uint32_t)da : 0u; if(pre>bytes) pre=bytes;
    if(pre) { memset(dst,value,pre); dst+=pre; bytes-=pre; }
    const uint32_t blocks=bytes>>4, vec=blocks<<4;
    if(blocks) { tab5_piegfx_fill8_blocks(dst,&value,blocks); }
    if(vec!=bytes) memset(dst+vec,value,bytes-vec);
}

int tab5_pie_graphics_diff(const void *va, const void *vb, uint32_t bytes)
{
    const uint8_t *a=(const uint8_t *)va, *b=(const uint8_t *)vb;
    if (!a || !b) return a!=b; if (!bytes) return 0;
    uintptr_t aa=(uintptr_t)a & 15u, ba=(uintptr_t)b & 15u;
    if (!s_piegfx_diff_ok || aa!=ba || bytes<64u) { return memcmp(a,b,bytes)!=0; }
    uint32_t pre=aa ? 16u-(uint32_t)aa : 0u; if(pre>bytes) pre=bytes;
    if (pre && memcmp(a,b,pre)!=0) return 1; a+=pre; b+=pre; bytes-=pre;
    const uint32_t blocks=bytes>>4, vec=blocks<<4;
    if (blocks) { if(tab5_piegfx_diff16_blocks(a,b,blocks)) return 1; }
    return vec==bytes ? 0 : (memcmp(a+vec,b+vec,bytes-vec)!=0);
}

void tab5_pie_graphics_init(void)
{
    if (s_piegfx_init_done) return; s_piegfx_init_done=1;

    /* Build 5.98g6: self-check scratch no longer occupies scarce internal
     * SRAM for the lifetime of the emulator.  PIE already operates on PSRAM
     * framebuffers in production, so use one temporary aligned PSRAM block
     * here and release it immediately after the one-shot checks. */
    enum { SCRATCH = 1280 };
    uint8_t *scratch = (uint8_t *)heap_caps_aligned_alloc(
        16, SCRATCH, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!scratch) {
        s_piegfx_copy_ok=s_piegfx_fill_ok=s_piegfx_diff_ok=0;
        printf("PX68K_PIEGFX598G6: self-check scratch PSRAM allocation FAIL; scalar safety fallback\n");
        return;
    }
    uint8_t *src = scratch + 0;
    uint8_t *dst = scratch + 256;
    uint8_t *eq = scratch + 512;
    uint16_t *fill = (uint16_t *)(void *)(scratch + 768);
    uint8_t *fill8 = scratch + 1024;

    for(uint32_t i=0;i<256u;++i) src[i]=(uint8_t)(i*29u+7u);
    memset(dst,0,256u); tab5_piegfx_copy16_blocks(dst,src,16u);
    s_piegfx_copy_ok = memcmp(dst,src,256u)==0;
    for(uint32_t i=0;i<128u;++i) fill[i]=0;
    const uint16_t fv=0x5aa5u; tab5_piegfx_fill16_blocks(fill,&fv,16u);
    s_piegfx_fill_ok=1; for(uint32_t i=0;i<128u;++i) if(fill[i]!=fv){s_piegfx_fill_ok=0;break;}
    memset(fill8,0,256u); { const uint8_t f8=0xa5u; tab5_piegfx_fill8_blocks(fill8,&f8,16u);
        for(uint32_t i=0;i<256u;++i) if(fill8[i]!=f8){s_piegfx_fill_ok=0;break;} }
    memcpy(eq,src,256u);
    const int deq=tab5_piegfx_diff16_blocks(src,eq,16u);
    eq[137]^=0x40u; const int dne=tab5_piegfx_diff16_blocks(src,eq,16u);
    s_piegfx_diff_ok = (deq==0 && dne==1);
    const int keyok = pie_synthetic_selfcheck();
    heap_caps_free(scratch);
    printf("PX68K_PIEGFX598G6: self-check COPY=%s FILL8/16=%s DIFF=%s KEY0=%s; primitives armed; scratch=PSRAM-temporary\n",
           s_piegfx_copy_ok?"PASS":"FAIL", s_piegfx_fill_ok?"PASS":"FAIL",
           s_piegfx_diff_ok?"PASS":"FAIL", keyok?"PASS":"FAIL");
}


void *tab5_ppa_alloc_framebuffer(size_t bytes)
{
    /* Kept ABI name: this is now simply the aligned PSRAM framebuffer allocator. */
    return heap_caps_aligned_alloc(128, bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

int tab5_p4blend_key0_overlay(uint16_t *frame_base, uint32_t frame_w,
                              uint32_t frame_h, uint32_t y,
                              const uint16_t *top_base, uint32_t top_pic_w,
                              uint32_t top_offset_x, uint32_t width)
{
    if (!frame_base || !top_base || !width || width > P4BLEND_MAX_WIDTH ||
        width > frame_w || y >= frame_h || top_offset_x + width > top_pic_w)
        return 0;

    uint16_t *dst = frame_base + (size_t)y * frame_w;
    const uint16_t *top = top_base + top_offset_x;
    if (pie_synthetic_selfcheck())
        pie_overlay_key0(dst, top, width);
    else
        scalar_overlay_key0(dst, top, width);
    return 1;
}

/* Retained no-op ABI for portable PX68K/windraw; RC builds no longer sample. */
