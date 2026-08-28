#pragma once
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif

enum {
    TAB5_R57_DOMAIN_TVRAM    = 1,
    TAB5_R57_DOMAIN_SPRITE   = 2,
    TAB5_R57_DOMAIN_BGREG    = 3,
    TAB5_R57_DOMAIN_BGMEM    = 4,
    TAB5_R57_DOMAIN_BG_RESET = 5,
    TAB5_R57_DOMAIN_RASTER   = 6,
};

int tab5_guest_bus_init(void);
void tab5_guest_bus_post_write(uint32_t domain, uint32_t address, uint8_t value);
void tab5_guest_bus_post_raster(uint32_t vline);

/* R57d: register an ordered freeze point BEFORE publishing its raster record.
 * CPU1 never waits. Return 0 if the bounded hold queue cannot accept it. */
uint32_t tab5_guest_bus_post_raster_hold(uint32_t vline);
void tab5_guest_bus_shadow_hold_cancel(uint32_t seq);
int tab5_guest_bus_shadow_hold_wait(uint32_t seq);
void tab5_guest_bus_shadow_hold_release(uint32_t seq);

int tab5_guest_bus_bg_shadow_ready(void);
const uint8_t *tab5_guest_bus_bg_shadow(void);
const uint8_t *tab5_guest_bus_bgchr8_shadow(void);
const uint8_t *tab5_guest_bus_bgchr16_shadow(void);
const uint8_t *tab5_guest_bus_sprite_shadow(void);
const uint8_t *tab5_guest_bus_bgreg_shadow(void);
uint32_t tab5_guest_bus_shadow_seq(void);

#ifdef __cplusplus
}
#endif
