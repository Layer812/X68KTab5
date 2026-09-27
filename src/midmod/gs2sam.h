#ifndef GS2SAM_H
#define GS2SAM_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define GS2SAM_SYSEX_MAX 256u
#define GS2SAM_DRUM_NOTE_DROP 0xFFu
#define GS2SAM_DRUM_RX_INHERIT 0xFFu
#define GS2SAM_BANK_LSB_ANY 0xFFu

/* Optional per-tone approximation flags. Values are SAM2695 GS NRPN
 * modifiers, encoded as signed offsets from neutral 64. */
enum {
    GS2SAM_TONE_VIB_RATE   = 1u << 0,
    GS2SAM_TONE_VIB_DEPTH  = 1u << 1,
    GS2SAM_TONE_VIB_DELAY  = 1u << 2,
    GS2SAM_TONE_CUTOFF     = 1u << 3,
    GS2SAM_TONE_RESONANCE  = 1u << 4,
    GS2SAM_TONE_ATTACK     = 1u << 5,
    GS2SAM_TONE_DECAY      = 1u << 6,
    GS2SAM_TONE_RELEASE    = 1u << 7
};

enum {
    /* If a source note has no explicit note rule, suppress it rather than
     * producing an unrelated percussion sound on the target kit. Useful for
     * the SC-55mkII SFX kit when retargeted to SAM2695 CM-64/32 partial. */
    GS2SAM_DRUM_KIT_DROP_UNMAPPED = 1u << 0
};

typedef struct gs2sam_tone_rule {
    uint8_t src_bank_msb;      /* GS CC#0 */
    uint8_t src_program;       /* MIDI program, 0..127 */
    uint8_t dst_bank_msb;      /* 0=GM capital, 127=SAM MT-32 map */
    uint8_t dst_program;       /* MIDI program, 0..127 */
    uint8_t flags;
    int8_t vibrato_rate;
    int8_t vibrato_depth;
    int8_t vibrato_delay;
    int8_t cutoff;
    int8_t resonance;
    int8_t attack;
    int8_t decay;
    int8_t release;
    uint8_t src_bank_lsb;      /* 0..127, or GS2SAM_BANK_LSB_ANY */
} gs2sam_tone_rule_t;

/* V5 ordered fallback for an unsupported GS variation bank. Exact tone
 * rules always win; native target banks 0/127 pass through; this family map
 * is consulted only for the remaining non-native bank values before the
 * generic GM-capital fallback. */
typedef struct gs2sam_tone_family_rule {
    uint8_t src_program;       /* source MIDI program, 0..127 */
    uint8_t dst_bank_msb;      /* target bank, normally 127 */
    uint8_t dst_program;       /* target MIDI program, 0..127 */
} gs2sam_tone_family_rule_t;

typedef struct gs2sam_drum_kit_rule {
    uint8_t src_program;       /* MIDI program, zero-based */
    uint8_t dst_program;       /* SAM2695 drum program, zero-based */
    uint8_t flags;
} gs2sam_drum_kit_rule_t;

typedef struct gs2sam_drum_note_rule {
    uint8_t src_program;       /* source drum kit program, zero-based */
    uint8_t src_note;
    uint8_t dst_note;          /* 0..127, or GS2SAM_DRUM_NOTE_DROP */
} gs2sam_drum_note_rule_t;

typedef void (*gs2sam_emit_fn)(void *user, const uint8_t *bytes, size_t len);

typedef struct gs2sam_config {
    gs2sam_emit_fn emit;
    void *emit_user;

    /* Unknown / malformed SysEx is passed through by default. */
    uint8_t pass_unknown_sysex;
    uint8_t pass_malformed_sysex;

    /* Unsupported GS melodic banks fall back to GM capital tone. */
    uint8_t capital_tone_fallback;

    /* Unsupported GS drum kits fall back to SAM Standard Set. */
    uint8_t drum_fallback;

    /* R3: apply source-profile drum-kit / drum-note retarget rules. */
    uint8_t drum_retarget;

    /* GS Drum1/Drum2 are shared maps. Fan a kit change out to every MIDI
     * channel currently assigned to the same logical GS drum map. */
    uint8_t emulate_shared_drum_maps;

    /* Emulate explicit GS per-note Rx Note On / Rx Note Off settings. */
    uint8_t emulate_drum_rx;

    /* Software emulation for GS features SAM2695 does not implement. */
    uint8_t emulate_key_range;

    /* Optional mix compensation for rhythm parts. 100 = unchanged.
     * Applied to CC#7 / GS Part Level, never to note velocity, so drum
     * dynamics are preserved. */
    uint8_t rhythm_volume_percent;

    const gs2sam_tone_rule_t *tone_rules;
    size_t tone_rule_count;
    const gs2sam_tone_family_rule_t *tone_family_rules;
    size_t tone_family_rule_count;
    const gs2sam_drum_kit_rule_t *drum_kit_rules;
    size_t drum_kit_rule_count;
    const gs2sam_drum_note_rule_t *drum_note_rules;
    size_t drum_note_rule_count;
} gs2sam_config_t;

typedef struct gs2sam_stats {
    uint32_t bytes_in;
    uint32_t bytes_out;
    uint32_t channel_messages;
    uint32_t sysex_messages;
    uint32_t native_pass;
    uint32_t exact_translate;
    uint32_t approximated;
    uint32_t unsupported;
    uint32_t malformed_sysex;
    uint32_t tone_fallbacks;
    uint32_t tone_family_fallbacks;
    uint32_t drum_fallbacks;
    uint32_t map2_collapses;
    uint32_t tone_sysex_translates;
    uint32_t tone_modify_translates;
    uint32_t key_range_updates;
    uint32_t key_range_filtered;
    uint32_t key_range_conflicts;

    /* R3 drum-retarget telemetry. */
    uint32_t drum_kit_retargets;
    uint32_t drum_note_remaps;
    uint32_t drum_note_drops;
    uint32_t drum_note_mapping_conflicts;
    uint32_t drum_setup_note_remaps;
    uint32_t drum_setup_note_drops;
    uint32_t drum_map_fanouts;
    uint32_t drum_map_syncs;
    uint32_t drum_rx_updates;
    uint32_t drum_rx_filtered;
    uint32_t rhythm_volume_scales;
} gs2sam_stats_t;

typedef struct gs2sam {
    gs2sam_config_t cfg;
    gs2sam_stats_t stats;

    uint8_t running_status;
    uint8_t msg_status;
    uint8_t msg_data[2];
    uint8_t msg_have;
    uint8_t msg_need;
    uint8_t system_common;

    uint8_t in_sysex;
    uint8_t sysex_passthrough;
    uint16_t sysex_len;
    uint8_t sysex[GS2SAM_SYSEX_MAX];

    uint8_t bank_msb[16];
    uint8_t bank_lsb[16];
    uint8_t target_bank_msb[16];

    /* Source/target CC#7 shadow state used by optional rhythm mix trim. */
    uint8_t source_volume[16];
    uint8_t target_volume[16];
    uint8_t target_volume_valid[16];

    /* GS block numbering: 0=Part10, 1..9=Part1..9, A..F=Part11..16. */
    uint8_t part_channel[16];   /* 0..15, 16=OFF */
    uint8_t part_rhythm_map[16];/* 0=melodic, 1=MAP1, 2=MAP2 */
    uint8_t part_key_low[16];
    uint8_t part_key_high[16];

    /* Logical SC-55 Drum1 / Drum2 source kit programs (zero-based). */
    uint8_t rhythm_map_program[2];

    /* 0/1 when explicitly set by GS drum setup SysEx; 0xFF means inherit
     * the target kit's native behavior. Indexed [map0/1][source note]. */
    uint8_t drum_rx_note_on[2][128];
    uint8_t drum_rx_note_off[2][128];

    /* Outstanding admitted Note Ons per channel/source-note. This ensures a
     * Note Off can still escape after later key-range changes. */
    uint8_t active_note_count[16][128];

    /* Target note chosen when the source note was admitted. Needed when a
     * drum kit is switched between Note On and Note Off. */
    uint8_t active_note_target[16][128];
} gs2sam_t;

void gs2sam_default_config(gs2sam_config_t *cfg,
                           gs2sam_emit_fn emit,
                           void *emit_user);

void gs2sam_init(gs2sam_t *s, const gs2sam_config_t *cfg);
void gs2sam_reset(gs2sam_t *s);
void gs2sam_feed(gs2sam_t *s, uint8_t byte);
void gs2sam_feed_buffer(gs2sam_t *s, const uint8_t *bytes, size_t len);
const gs2sam_stats_t *gs2sam_get_stats(const gs2sam_t *s);

/* Roland checksum for address+data bytes (not including F0..12 or F7). */
uint8_t gs2sam_roland_checksum(const uint8_t *bytes, size_t len);

#ifdef __cplusplus
}
#endif

#endif /* GS2SAM_H */
