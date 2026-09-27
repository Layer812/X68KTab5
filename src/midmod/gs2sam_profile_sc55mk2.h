#ifndef GS2SAM_PROFILE_SC55MK2_H
#define GS2SAM_PROFILE_SC55MK2_H

#include "gs2sam.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Conservative, high-confidence SC-55mkII variation retargets. */
const gs2sam_tone_rule_t *gs2sam_sc55mk2_tone_rules(size_t *count);

/* V5 same-family fallback for otherwise unsupported GS variation banks. */
const gs2sam_tone_family_rule_t *gs2sam_sc55mk2_tone_family_rules(size_t *count);

/* R3 drum retarget profile. MIDI program numbers are zero-based.
 * Source SC-55mkII kits: 1/9/17/25/26/33/41/49/57/128.
 * SAM2695 native kits: 1/17/41/49/128. */
const gs2sam_drum_kit_rule_t *gs2sam_sc55mk2_drum_kit_rules(size_t *count);
const gs2sam_drum_note_rule_t *gs2sam_sc55mk2_drum_note_rules(size_t *count);

/* Convenience config: default engine policy + SC-55mkII tone/drum rules. */
void gs2sam_sc55mk2_config(gs2sam_config_t *cfg,
                           gs2sam_emit_fn emit,
                           void *emit_user);

#ifdef __cplusplus
}
#endif

#endif /* GS2SAM_PROFILE_SC55MK2_H */
