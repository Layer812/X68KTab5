#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VgmM5YM2151 VgmM5YM2151;

VgmM5YM2151 *vgmm5_ym2151_create(uint32_t clock, uint32_t sample_rate);
void vgmm5_ym2151_destroy(VgmM5YM2151 *ym);
void vgmm5_ym2151_reset(VgmM5YM2151 *ym);
void vgmm5_ym2151_write(VgmM5YM2151 *ym, uint8_t reg, uint8_t data);
void vgmm5_ym2151_set_px_volume(VgmM5YM2151 *ym, uint8_t vol);
void vgmm5_ym2151_csm_pulse(VgmM5YM2151 *ym);
void vgmm5_ym2151_render(VgmM5YM2151 *ym, int16_t *stereo, uint32_t frames);

#ifdef __cplusplus
}
#endif
