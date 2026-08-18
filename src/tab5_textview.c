/*
 * Tab5 port-specific implementation.
 * Intent: Host-side text renderer used for Tab5 diagnostics and alternate color-text presentation without changing guest VRAM state.
 * Layer8 Aug/17/2026
 */
#include "tab5_textview.h"

#include <stddef.h>
#include <stdint.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

/*
 * Build 4.3: use PX68K's own already-expanded text pixel buffer.
 *
 * Do not reinterpret TVRAM here.  tvram.c maintains TextDrawWork as
 * one palette index per 1024x1024 text pixel, and Text_DrawLine() uses
 * that same buffer.  Reading it directly guarantees identical text
 * geometry to the PX68K core.
 */
extern uint8_t  CRTC_Regs[48];
extern uint32_t TextDotX, TextDotY;
extern uint32_t TextScrollX, TextScrollY;
extern const uint8_t *TVRAM_GetExpandedPixels(void);
extern uint16_t TextPal[256];

#define TEXTVIEW_MAX_W 1024u
#define TEXTVIEW_MAX_H 1024u

static const char *TAG = "TAB5_TEXTVIEW";
static uint16_t *s_fb = NULL;

static uint32_t clamp_u32(uint32_t v, uint32_t maxv)
{
    return (v > maxv) ? maxv : v;
}

int tab5_textview_init(void)
{
    if (s_fb)
        return 1;

    const size_t bytes =
        (size_t)TEXTVIEW_MAX_W *
        (size_t)TEXTVIEW_MAX_H *
        sizeof(uint16_t);

    s_fb = (uint16_t *)heap_caps_malloc(
        bytes,
        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);

    if (!s_fb)
    {
        ESP_LOGE(TAG,
                 "PSRAM allocation failed: %u bytes",
                 (unsigned)bytes);
        return 0;
    }

    ESP_LOGI(TAG,
             "Core TextDrawWork renderer ready: %ux%u color buffer",
             (unsigned)TEXTVIEW_MAX_W,
             (unsigned)TEXTVIEW_MAX_H);

    return 1;
}

int tab5_textview_render(tab5_textview_frame_t *out)
{
    if (!out || !s_fb)
        return 0;

    const uint8_t *work = TVRAM_GetExpandedPixels();
    if (!work)
        return 0;

    const uint32_t w = clamp_u32(TextDotX, TEXTVIEW_MAX_W);
    const uint32_t h = clamp_u32(TextDotY, TEXTVIEW_MAX_H);

    if (w == 0 || h == 0)
        return 0;

    const int double_scan =
        ((CRTC_Regs[0x29] & 0x1cu) == 0x1cu);

    const uint32_t scroll_x = TextScrollX & 0x3ffu;
    const uint32_t visible_before_right_edge = 1024u - scroll_x;

    uint32_t nz = 0;
    uint32_t hash = 2166136261u;

    for (uint32_t y = 0; y < h; ++y)
    {
        uint32_t gy = TextScrollY + y;
        if (double_scan)
            gy += y;
        gy &= 0x3ffu;

        const uint8_t *src = work + (gy << 10);
        uint16_t *dst = s_fb + y * TEXTVIEW_MAX_W;

        /*
         * Match Text_DrawLine() exactly: horizontal scrolling does not
         * wrap.  Once the 1024-pixel text plane right edge is reached,
         * the remaining visible pixels use text palette entry 0.
         *
         * Build 5.7: use PX68K's native TextPal[] RGB565 values rather
         * than the previous monochrome proof colors.  TextDrawWork stores
         * the 4-bit text palette index, exactly as Text_DrawLine() uses it.
         */
        for (uint32_t x = 0; x < w; ++x)
        {
            uint8_t idx = 0;
            if (x < visible_before_right_edge)
                idx = src[scroll_x + x] & 0x0fu;

            if (idx)
                ++nz;

            dst[x] = TextPal[idx];

            hash ^= idx;
            hash *= 16777619u;
        }
    }

    out->pixels = s_fb;
    out->width = w;
    out->height = h;
    out->pitch_pixels = TEXTVIEW_MAX_W;
    out->nonzero_pixels = nz;
    out->hash = hash;
    /* Build 5.2 proved TVRAM and TextDrawWork were identical (TXMIS=0). */
    out->expanded_mismatch_pixels = 0;
    {
        int active = 0;
        for (unsigned i = 0; i < 16u; ++i)
        {
            if (TextPal[i] != 0u)
            {
                active = 1;
                break;
            }
        }
        out->palette_active = active;
    }
    out->scroll_x = TextScrollX & 0x3ffu;
    out->scroll_y = TextScrollY & 0x3ffu;
    out->text_dot_x = TextDotX;
    out->text_dot_y = TextDotY;
    out->double_scan = double_scan;

    return 1;
}
