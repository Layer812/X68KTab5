#ifndef GS2SAM_PROCESSOR_H
#define GS2SAM_PROCESSOR_H

#include <stdint.h>

#include <midi_engine.h>
#include "gs2sam.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct gs2sam_processor {
    midi_engine_t *engine;
    gs2sam_t core;
    gs2sam_config_t config;
} gs2sam_processor_t;

/* GS2SAM is only one processor implementation. The generic MIDI engine does
 * not depend on it and may instead run pass-through or another processor. */
void gs2sam_processor_init_sc55mk2(gs2sam_processor_t *p,
                                   midi_engine_t *engine,
                                   uint8_t rhythm_volume_percent);

void gs2sam_processor_apply_sc55mk2(gs2sam_processor_t *p,
                                    uint8_t rhythm_volume_percent);

void gs2sam_processor_apply_config(gs2sam_processor_t *p,
                                   const gs2sam_config_t *config);


midi_processor_t gs2sam_processor_interface(gs2sam_processor_t *p);
const gs2sam_stats_t *gs2sam_processor_stats(const gs2sam_processor_t *p);

#ifdef __cplusplus
}
#endif

#endif /* GS2SAM_PROCESSOR_H */
