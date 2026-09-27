#include "gs2sam_profile_sc55mk2.h"

/* Built-in SC55 profile for X68K Tab Production V5.
 * Source variations are the 92 documented SC-55/SC-55mkII rows in
 * sc55mk2_variation_map_r2.csv.  High-confidence matches retarget
 * to SAM2695 Bank 127 or GM equivalents; every remaining known
 * variation is made explicit as a deterministic fallback. V5 adds
 * semantic family fallbacks for undocumented GS banks. No synthetic
 * NRPN timbre guesses are introduced here.
 *
 * MIDI program numbers below are zero-based.  Bank LSB is wildcard.
 */

static const gs2sam_tone_rule_t k_sc55_rules[] = {
    {   8,   0,   0,   0, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Piano 1w -> GM capital same PC [capital_fallback] */
    {  16,   0,   0,   0, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Piano 1d -> GM capital same PC [capital_fallback] */
    {   8,   1,   0,   1, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Piano 2w -> GM capital same PC [capital_fallback] */
    {   8,   2,   0,   2, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Piano 3w -> GM capital same PC [capital_fallback] */
    {   8,   3,   0,   3, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Honky-tonk w -> GM capital same PC [capital_fallback] */
    {   8,   4, 127,   3, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Detuned EP 1 -> Detuned EP 1 [profile_retarget] */
    {  16,   4,   0,   4, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* E. Piano 1v -> GM capital same PC [capital_fallback] */
    {  24,   4,   0,   4, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* 60's E. Piano -> GM capital same PC [capital_fallback] */
    {   8,   5, 127,   6, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Detuned EP 2 -> Detuned EP 2 [profile_retarget] */
    {  16,   5,   0,   5, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* E. Piano 2v -> GM capital same PC [capital_fallback] */
    {   8,   6, 127,  17, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Coupled Hps. -> Coupled Hps. [profile_retarget] */
    {  16,   6,   0,   6, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Harpsi.w -> GM capital same PC [capital_fallback] */
    {  24,   6,   0,   6, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Harpsi.o -> GM capital same PC [capital_fallback] */
    {   8,  11,   0,  11, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Vib.w -> GM capital same PC [capital_fallback] */
    {   8,  12,   0,  12, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Marimba w -> GM capital same PC [capital_fallback] */
    {   8,  14,   0,  14, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Church Bell -> GM capital same PC [capital_fallback] */
    {   9,  14,   0,  14, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Carillon -> GM capital same PC [capital_fallback] */
    {   8,  16, 127,  11, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Detuned Or. 1 -> Detuned Or. 1 [profile_retarget] */
    {  16,  16,   0,  16, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* 60's Organ 1 -> GM capital same PC [capital_fallback] */
    {  32,  16,   0,  16, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Organ 4 -> GM capital same PC [capital_fallback] */
    {   8,  17,   0,  17, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Detuned Or. 2 -> GM capital same PC [capital_fallback] */
    {  32,  17,   0,  17, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Organ 5 -> GM capital same PC [capital_fallback] */
    {   8,  19, 127,  12, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Church Org.2 -> Church Org. 2 [profile_retarget] */
    {  16,  19,   0,  19, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Church Org.3 -> GM capital same PC [capital_fallback] */
    {   8,  21,   0,  21, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Accordion It -> GM capital same PC [capital_fallback] */
    {   8,  24,   0,  24, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Ukulele -> GM capital same PC [capital_fallback] */
    {  16,  24,   0,  24, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Nylon Gt.o -> GM capital same PC [capital_fallback] */
    {  32,  24,   0,  24, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Nylon Gt.2 -> GM capital same PC [capital_fallback] */
    {   8,  25,   0,  25, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* 12-str. Gt. -> GM capital same PC [capital_fallback] */
    {  16,  25,   0,  25, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Mandolin -> GM capital same PC [capital_fallback] */
    {   8,  26,   0,  26, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Hawaiian Gt. -> GM capital same PC [capital_fallback] */
    {   8,  27, 127,  61, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Chorus Gt. -> Chorus Gt. [profile_retarget] */
    {   8,  28, 127,  62, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Funk Gt. -> Funk Gt. [profile_retarget] */
    {  16,  28,   0,  28, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Funk Gt.2 -> GM capital same PC [capital_fallback] */
    {   8,  30,   0,  30, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Feedback Gt. -> GM capital same PC [capital_fallback] */
    {   8,  31,   0,  31, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Gt. Feedback -> GM capital same PC [capital_fallback] */
    {   1,  38, 127,  28, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Synth Bass 101 -> MT-32 Synth Bass1 [semantic_retarget] */
    {   8,  38, 127,  30, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Synth Bass 3 -> Synth Bass 3 [profile_retarget] */
    {   8,  39, 127,  31, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Synth Bass 4 -> Synth Bass 4 [profile_retarget] */
    {  16,  39, 127,  29, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Rubber Bass -> MT-32 Synth Bass2 [semantic_retarget] */
    {   8,  40,   0,  40, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Slow Violin -> GM capital same PC [capital_fallback] */
    {   8,  48,   0,  48, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Orchestra -> GM capital same PC [capital_fallback] */
    {   8,  50,   0,  50, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Syn. Strings3 -> GM capital same PC [capital_fallback] */
    {  32,  52, 127,  34, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Choir Aahs 2 -> MT-32 Choir Aahs [semantic_retarget] */
    {   1,  57, 127,  91, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Trombone 2 -> MT-32 Trombone #2 [semantic_retarget] */
    {   1,  60, 127,  92, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Fr. Horn -> French Horn [profile_retarget] */
    {   8,  61, 127,  96, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Brass 2 -> Brass 2 [profile_retarget] */
    {   8,  62, 127,  26, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Synth Brass3 -> Synth Brass3 [profile_retarget] */
    {  16,  62, 127,  24, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* AnalogBrass1 -> MT-32 Synth Brass1 [semantic_retarget] */
    {   8,  63, 127,  27, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Synth Brass4 -> Synth Brass4 [profile_retarget] */
    {  16,  63, 127,  25, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* AnalogBrass2 -> MT-32 Synth Brass2 [semantic_retarget] */
    {   1,  80, 127,  47, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Square -> Square Wave [profile_retarget] */
    {   8,  80,   0,  80, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Sine Wave -> GM capital same PC [capital_fallback] */
    {   1,  81, 127,  44, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Saw -> Saw Wave [profile_retarget] */
    {   8,  81, 127,  44, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Doctor Solo -> MT-32 Saw Wave [semantic_retarget] */
    {   8, 107, 127, 106, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Taisho Koto -> Taisho Koto [profile_retarget] */
    {   8, 115, 127, 120, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Castanets -> Castanets [profile_retarget] */
    {   8, 116,   0, 116, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Concert BD -> GM capital same PC [capital_fallback] */
    {   8, 117, 127, 114, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Melo Tom 2 -> MT-32 Melo Tom [semantic_retarget] */
    {   8, 118,   0, 118, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* 808 Tom -> GM capital same PC [capital_fallback] */
    {   1, 120,   0, 120, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Gt. Cut Noise -> GM capital same PC [capital_fallback] */
    {   2, 120,   0, 120, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* String Slap -> GM capital same PC [capital_fallback] */
    {   1, 121,   0, 121, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Fl. Key Click -> GM capital same PC [capital_fallback] */
    {   1, 122,   0,  96, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Rain -> FX 1 (rain) [profile_retarget] */
    {   2, 122,   0, 122, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Thunder -> GM capital same PC [capital_fallback] */
    {   3, 122,   0, 122, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Wind -> GM capital same PC [capital_fallback] */
    {   4, 122,   0, 122, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Stream -> GM capital same PC [capital_fallback] */
    {   5, 122,   0, 122, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Bubble -> GM capital same PC [capital_fallback] */
    {   1, 123,   0, 123, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Dog -> GM capital same PC [capital_fallback] */
    {   2, 123,   0, 123, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Horse-Gallop -> GM capital same PC [capital_fallback] */
    {   1, 124,   0, 124, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Telephone 2 -> GM capital same PC [capital_fallback] */
    {   2, 124,   0, 124, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Door Creaking -> GM capital same PC [capital_fallback] */
    {   3, 124,   0, 124, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Door -> GM capital same PC [capital_fallback] */
    {   4, 124,   0, 124, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Scratch -> GM capital same PC [capital_fallback] */
    {   5, 124,   0, 124, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Windchime -> GM capital same PC [capital_fallback] */
    {   1, 125,   0, 125, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Car-Engine -> GM capital same PC [capital_fallback] */
    {   2, 125,   0, 125, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Car-Stop -> GM capital same PC [capital_fallback] */
    {   3, 125,   0, 125, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Car-Pass -> GM capital same PC [capital_fallback] */
    {   4, 125,   0, 125, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Car-Crash -> GM capital same PC [capital_fallback] */
    {   5, 125,   0, 125, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Siren -> GM capital same PC [capital_fallback] */
    {   6, 125,   0, 125, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Train -> GM capital same PC [capital_fallback] */
    {   7, 125,   0, 125, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Jetplane -> GM capital same PC [capital_fallback] */
    {   8, 125,   0, 125, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Starship -> GM capital same PC [capital_fallback] */
    {   9, 125,   0, 125, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Burst Noise -> GM capital same PC [capital_fallback] */
    {   1, 126,   0, 126, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Laughing -> GM capital same PC [capital_fallback] */
    {   2, 126,   0, 126, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Screaming -> GM capital same PC [capital_fallback] */
    {   3, 126,   0, 126, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Punch -> GM capital same PC [capital_fallback] */
    {   4, 126,   0, 126, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Heart Beat -> GM capital same PC [capital_fallback] */
    {   5, 126,   0, 126, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Footsteps -> GM capital same PC [capital_fallback] */
    {   1, 127,   0, 127, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Machine Gun -> GM capital same PC [capital_fallback] */
    {   2, 127,   0, 127, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Lasergun -> GM capital same PC [capital_fallback] */
    {   3, 127,   0, 127, 0, 0,0,0,0,0,0,0,0, GS2SAM_BANK_LSB_ANY }, /* Explosion -> GM capital same PC [capital_fallback] */
};

/* V5 family fallbacks are consulted only when no exact variation rule exists
 * and the source bank is neither target-native GM(0) nor MT-32(127). They
 * provide a deterministic same-family Bank-127 target before the generic
 * GM-capital collapse. */
static const gs2sam_tone_family_rule_t k_sc55_family_rules[] = {
    {  19, 127,  13 }, /* Church Organ -> Church Org. */
    {  38, 127,  28 }, /* Synth Bass 1 -> Synth Bass1 */
    {  39, 127,  29 }, /* Synth Bass 2 -> Synth Bass2 */
    {  52, 127,  34 }, /* Choir Aahs -> Choir Aahs */
    {  57, 127,  91 }, /* Trombone -> Trombone #2 */
    {  60, 127,  92 }, /* French Horn -> French Horn */
    {  61, 127,  96 }, /* Brass Section -> Brass 2 */
    {  62, 127,  24 }, /* Synth Brass 1 -> Synth Brass1 */
    {  63, 127,  25 }, /* Synth Brass 2 -> Synth Brass2 */
    {  80, 127,  47 }, /* Lead 1 family -> Square Wave */
    {  81, 127,  44 }, /* Lead 2 family -> Saw Wave */
    { 117, 127, 114 }, /* Melodic Tom family -> Melo Tom */
};

static const gs2sam_drum_kit_rule_t k_sc55_drum_kits[] = {
    {   0,   0, 0 }, /* STANDARD -> STANDARD */
    {   8,   0, 0 }, /* ROOM -> STANDARD */
    {  16,  16, 0 }, /* POWER -> POWER */
    {  24,  16, 0 }, /* ELECTRONIC -> POWER */
    {  25,   0, 0 }, /* TR-808 -> STANDARD */
    {  32,   0, 0 }, /* JAZZ -> STANDARD */
    {  40,  40, 0 }, /* BRUSH -> BRUSH */
    {  48,  48, 0 }, /* ORCHESTRA -> ORCHESTRA */
    {  56, 127, GS2SAM_DRUM_KIT_DROP_UNMAPPED }, /* SFX -> CM-64/32 partial */
    { 127, 127, 0 }, /* CM-64/32 -> CM-64/32 partial */
};

static const gs2sam_drum_note_rule_t k_sc55_drum_notes[] = {
    {  24,  38,  40 }, /* Elec SD -> Snare Drum 2 */
    {  24,  40,  38 }, /* Gated SD -> Gated Snare */
    {  56,  58,  82 }, /* Applause -> Applauses */
    {  56,  70,  94 }, /* Helicopter -> Helicopter */
    {  56,  72,  96 }, /* Gun Shot -> Gun Shot */
    {  56,  78, 102 }, /* Birds -> Birds */
    {  56,  82, 106 }, /* Seashore -> SeaShore */
};

const gs2sam_tone_rule_t *gs2sam_sc55mk2_tone_rules(size_t *count)
{
    if (count) *count = sizeof(k_sc55_rules) / sizeof(k_sc55_rules[0]);
    return k_sc55_rules;
}

const gs2sam_tone_family_rule_t *gs2sam_sc55mk2_tone_family_rules(size_t *count)
{
    if (count) *count = sizeof(k_sc55_family_rules) / sizeof(k_sc55_family_rules[0]);
    return k_sc55_family_rules;
}

const gs2sam_drum_kit_rule_t *gs2sam_sc55mk2_drum_kit_rules(size_t *count)
{
    if (count) *count = sizeof(k_sc55_drum_kits) / sizeof(k_sc55_drum_kits[0]);
    return k_sc55_drum_kits;
}

const gs2sam_drum_note_rule_t *gs2sam_sc55mk2_drum_note_rules(size_t *count)
{
    if (count) *count = sizeof(k_sc55_drum_notes) / sizeof(k_sc55_drum_notes[0]);
    return k_sc55_drum_notes;
}

void gs2sam_sc55mk2_config(gs2sam_config_t *cfg,
                           gs2sam_emit_fn emit,
                           void *emit_user)
{
    size_t n = 0;
    gs2sam_default_config(cfg, emit, emit_user);
    cfg->tone_rules = gs2sam_sc55mk2_tone_rules(&n);
    cfg->tone_rule_count = n;
    cfg->tone_family_rules = gs2sam_sc55mk2_tone_family_rules(&n);
    cfg->tone_family_rule_count = n;
    cfg->drum_kit_rules = gs2sam_sc55mk2_drum_kit_rules(&n);
    cfg->drum_kit_rule_count = n;
    cfg->drum_note_rules = gs2sam_sc55mk2_drum_note_rules(&n);
    cfg->drum_note_rule_count = n;
    cfg->rhythm_volume_percent = 100u;
}
