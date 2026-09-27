#ifndef GS2SAM_PROFILE_CSV_H
#define GS2SAM_PROFILE_CSV_H

#include "gs2sam.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-capacity profile storage: no heap allocation is required on target.
 * 1152 tone rules is enough for a large SC-88Pro-style remap table while
 * keeping ESP32-S3 RAM use predictable. */
#define GS2SAM_CSV_MAX_TONE_RULES 1152u
#define GS2SAM_CSV_MAX_TONE_FAMILY_RULES 128u
#define GS2SAM_CSV_MAX_DRUM_KIT_RULES 128u
#define GS2SAM_CSV_MAX_DRUM_NOTE_RULES 512u
#define GS2SAM_CSV_META_LEN 48u
#define GS2SAM_CSV_ERROR_LEN 112u

typedef struct gs2sam_csv_profile {
    char comment[GS2SAM_CSV_META_LEN];
    char name[GS2SAM_CSV_META_LEN];
    char source[GS2SAM_CSV_META_LEN];
    char target[GS2SAM_CSV_META_LEN];
    char status[GS2SAM_CSV_META_LEN];

    uint8_t pass_unknown_sysex;
    uint8_t pass_malformed_sysex;
    uint8_t capital_tone_fallback;
    uint8_t drum_fallback;
    uint8_t drum_retarget;
    uint8_t emulate_shared_drum_maps;
    uint8_t emulate_drum_rx;
    uint8_t emulate_key_range;
    uint8_t rhythm_volume_percent;
    uint8_t signature_seen;

    gs2sam_tone_rule_t tone_rules[GS2SAM_CSV_MAX_TONE_RULES];
    size_t tone_rule_count;
    gs2sam_tone_family_rule_t tone_family_rules[GS2SAM_CSV_MAX_TONE_FAMILY_RULES];
    size_t tone_family_rule_count;
    gs2sam_drum_kit_rule_t drum_kit_rules[GS2SAM_CSV_MAX_DRUM_KIT_RULES];
    size_t drum_kit_rule_count;
    gs2sam_drum_note_rule_t drum_note_rules[GS2SAM_CSV_MAX_DRUM_NOTE_RULES];
    size_t drum_note_rule_count;

    uint32_t lines_seen;
    uint32_t rows_loaded;
    uint32_t rows_ignored;
    uint32_t errors;
    char last_error[GS2SAM_CSV_ERROR_LEN];
} gs2sam_csv_profile_t;

/* Initialize a profile with the same safe policy defaults as GS2SAM. */
void gs2sam_csv_profile_init(gs2sam_csv_profile_t *profile);

/* Parse one mutable line. The first physical line, when it begins with #,
 * is stored (without # and surrounding whitespace) as the screen comment.
 * Other comments beginning with # and blank lines are ignored. Unknown row types are ignored for forward compatibility.
 * Returns 1 for a recognized row, 0 for ignored/comment, -1 for an error. */
int gs2sam_csv_profile_parse_line(gs2sam_csv_profile_t *profile,
                                  char *line,
                                  uint32_t line_no);

/* Build the live GS2SAM config using rules stored in profile. The profile
 * object must remain alive while GS2SAM uses the returned config pointers. */
void gs2sam_csv_profile_make_config(const gs2sam_csv_profile_t *profile,
                                    gs2sam_config_t *cfg,
                                    gs2sam_emit_fn emit,
                                    void *emit_user);

#ifdef __cplusplus
}
#endif

#endif /* GS2SAM_PROFILE_CSV_H */
