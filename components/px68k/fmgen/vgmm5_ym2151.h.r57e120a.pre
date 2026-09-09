#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct VgmM5YM2151 VgmM5YM2151;

typedef struct {
    uint32_t sampled_frames;
    uint32_t total_cycles;
    uint32_t envelope_cycles;
    uint32_t lfo_noise_cycles;
    uint32_t channel_prep_cycles;
    uint32_t operator_cycles;
    uint32_t routing_pan_cycles;
    uint32_t post_cycles;
} vgmm5_ym2151_profile_t;

VgmM5YM2151 *vgmm5_ym2151_create(uint32_t clock, uint32_t sample_rate);
void vgmm5_ym2151_destroy(VgmM5YM2151 *ym);
void vgmm5_ym2151_reset(VgmM5YM2151 *ym);
void vgmm5_ym2151_write(VgmM5YM2151 *ym, uint8_t reg, uint8_t data);
void vgmm5_ym2151_set_px_volume(VgmM5YM2151 *ym, uint8_t vol);
void vgmm5_ym2151_csm_pulse(VgmM5YM2151 *ym);
void vgmm5_ym2151_render(VgmM5YM2151 *ym, int16_t *stereo, uint32_t frames);
int vgmm5_ym2151_memory_internal(const VgmM5YM2151 *ym);
void vgmm5_ym2151_profile_get(const VgmM5YM2151 *ym, vgmm5_ym2151_profile_t *out);
void vgmm5_ym2151_semantic_algo_get(const VgmM5YM2151 *ym, uint32_t out[8]);

#ifdef __cplusplus
}
#endif
