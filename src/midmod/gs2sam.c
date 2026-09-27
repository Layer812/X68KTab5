#include "gs2sam.h"

#include <string.h>

#define MIDI_CH_STATUS(s) ((uint8_t)((s) & 0xF0u))
#define MIDI_CH(s)        ((uint8_t)((s) & 0x0Fu))

static uint8_t clamp127(int v)
{
    if (v < 0) return 0;
    if (v > 127) return 127;
    return (uint8_t)v;
}

static void emit_raw(gs2sam_t *s, const uint8_t *p, size_t n)
{
    if (!n || !s->cfg.emit) return;
    s->cfg.emit(s->cfg.emit_user, p, n);
    s->stats.bytes_out += (uint32_t)n;
}

static void emit1(gs2sam_t *s, uint8_t a)
{
    emit_raw(s, &a, 1);
}

static void emit2(gs2sam_t *s, uint8_t a, uint8_t b)
{
    uint8_t x[2] = {a, b};
    emit_raw(s, x, 2);
}

static void emit3(gs2sam_t *s, uint8_t a, uint8_t b, uint8_t c)
{
    uint8_t x[3] = {a, b, c};
    emit_raw(s, x, 3);
}

static void emit_cc(gs2sam_t *s, uint8_t ch, uint8_t cc, uint8_t value)
{
    emit3(s, (uint8_t)(0xB0u | ch), cc, value);
}

static void emit_pc(gs2sam_t *s, uint8_t ch, uint8_t program)
{
    emit2(s, (uint8_t)(0xC0u | ch), program);
}

static void emit_nrpn(gs2sam_t *s, uint8_t ch, uint8_t msb, uint8_t lsb, uint8_t value)
{
    emit_cc(s, ch, 99, msb);
    emit_cc(s, ch, 98, lsb);
    emit_cc(s, ch, 6, value);
    /* Null NRPN selection avoids later Data Entry changing it accidentally. */
    emit_cc(s, ch, 99, 127);
    emit_cc(s, ch, 98, 127);
}

static void emit_rpn(gs2sam_t *s, uint8_t ch, uint8_t msb, uint8_t lsb, uint8_t value)
{
    emit_cc(s, ch, 101, msb);
    emit_cc(s, ch, 100, lsb);
    emit_cc(s, ch, 6, value);
    emit_cc(s, ch, 101, 127);
    emit_cc(s, ch, 100, 127);
}

uint8_t gs2sam_roland_checksum(const uint8_t *bytes, size_t len)
{
    unsigned sum = 0;
    size_t i;
    for (i = 0; i < len; ++i) sum += bytes[i];
    return (uint8_t)((128u - (sum & 0x7Fu)) & 0x7Fu);
}

static void reset_gs_state(gs2sam_t *s)
{
    unsigned ch, p;
    memset(s->bank_msb, 0, sizeof(s->bank_msb));
    memset(s->bank_lsb, 0, sizeof(s->bank_lsb));
    memset(s->target_bank_msb, 0, sizeof(s->target_bank_msb));
    memset(s->source_volume, 100, sizeof(s->source_volume));
    memset(s->target_volume, 0, sizeof(s->target_volume));
    memset(s->target_volume_valid, 0, sizeof(s->target_volume_valid));

    /* SAM2695 / GS default block-to-channel assignment from datasheet. */
    s->part_channel[0] = 9; /* Part10 / drums */
    for (ch = 1; ch <= 9; ++ch) s->part_channel[ch] = (uint8_t)(ch - 1);
    for (ch = 10; ch <= 15; ++ch) s->part_channel[ch] = (uint8_t)ch;

    memset(s->part_rhythm_map, 0, sizeof(s->part_rhythm_map));
    s->part_rhythm_map[0] = 1; /* Part10 = MAP1 */
    s->rhythm_map_program[0] = 0; /* Standard */
    s->rhythm_map_program[1] = 0; /* Standard */
    memset(s->drum_rx_note_on, GS2SAM_DRUM_RX_INHERIT, sizeof(s->drum_rx_note_on));
    memset(s->drum_rx_note_off, GS2SAM_DRUM_RX_INHERIT, sizeof(s->drum_rx_note_off));

    for (p = 0; p < 16; ++p) {
        s->part_key_low[p] = 0;
        s->part_key_high[p] = 127;
    }
    memset(s->active_note_count, 0, sizeof(s->active_note_count));
    memset(s->active_note_target, GS2SAM_DRUM_NOTE_DROP, sizeof(s->active_note_target));
}

void gs2sam_default_config(gs2sam_config_t *cfg,
                           gs2sam_emit_fn emit,
                           void *emit_user)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->emit = emit;
    cfg->emit_user = emit_user;
    cfg->pass_unknown_sysex = 1;
    cfg->pass_malformed_sysex = 1;
    cfg->capital_tone_fallback = 1;
    cfg->drum_fallback = 1;
    cfg->drum_retarget = 1;
    cfg->emulate_shared_drum_maps = 1;
    cfg->emulate_drum_rx = 1;
    cfg->emulate_key_range = 1;
    cfg->rhythm_volume_percent = 100;
}

void gs2sam_init(gs2sam_t *s, const gs2sam_config_t *cfg)
{
    memset(s, 0, sizeof(*s));
    if (cfg) s->cfg = *cfg;
    reset_gs_state(s);
}

void gs2sam_reset(gs2sam_t *s)
{
    gs2sam_config_t cfg = s->cfg;
    memset(s, 0, sizeof(*s));
    s->cfg = cfg;
    reset_gs_state(s);
}

const gs2sam_stats_t *gs2sam_get_stats(const gs2sam_t *s)
{
    return &s->stats;
}

static uint8_t channel_rhythm_map(const gs2sam_t *s, uint8_t ch, int *conflict)
{
    unsigned p;
    uint8_t map = 0;
    if (conflict) *conflict = 0;
    for (p = 0; p < 16; ++p) {
        if (s->part_channel[p] != ch || s->part_rhythm_map[p] == 0) continue;
        if (map == 0) map = s->part_rhythm_map[p];
        else if (map != s->part_rhythm_map[p]) {
            if (conflict) *conflict = 1;
        }
    }
    return map;
}

static int channel_is_rhythm(const gs2sam_t *s, uint8_t ch)
{
    return channel_rhythm_map(s, ch, NULL) != 0;
}

static uint8_t target_part_volume(const gs2sam_t *s, uint8_t ch, uint8_t source)
{
    unsigned pct = s->cfg.rhythm_volume_percent;
    unsigned scaled;
    if (pct > 100u) pct = 100u;
    if (!channel_is_rhythm(s, ch) || pct == 100u) return source;
    scaled = ((unsigned)source * pct + 50u) / 100u;
    if (scaled > 127u) scaled = 127u;
    return (uint8_t)scaled;
}

static void emit_part_volume(gs2sam_t *s, uint8_t ch, uint8_t source, int force)
{
    uint8_t target;
    if (ch >= 16) return;
    s->source_volume[ch] = source;
    target = target_part_volume(s, ch, source);
    if (!force && s->target_volume_valid[ch] && s->target_volume[ch] == target)
        return;
    emit_cc(s, ch, 7, target);
    s->target_volume[ch] = target;
    s->target_volume_valid[ch] = 1;
    if (target != source) s->stats.rhythm_volume_scales++;
}

static void ensure_part_volume(gs2sam_t *s, uint8_t ch)
{
    uint8_t target;
    if (ch >= 16) return;
    if (!channel_is_rhythm(s, ch) || s->cfg.rhythm_volume_percent >= 100u) return;
    target = target_part_volume(s, ch, s->source_volume[ch]);
    if (!s->target_volume_valid[ch] || s->target_volume[ch] != target)
        emit_part_volume(s, ch, s->source_volume[ch], 1);
}

static const gs2sam_drum_kit_rule_t *find_drum_kit_rule(const gs2sam_t *s,
                                                         uint8_t program)
{
    size_t i;
    for (i = 0; i < s->cfg.drum_kit_rule_count; ++i) {
        const gs2sam_drum_kit_rule_t *r = &s->cfg.drum_kit_rules[i];
        if (r->src_program == program) return r;
    }
    return NULL;
}

static const gs2sam_drum_note_rule_t *find_drum_note_rule(const gs2sam_t *s,
                                                           uint8_t program,
                                                           uint8_t note)
{
    size_t i;
    for (i = 0; i < s->cfg.drum_note_rule_count; ++i) {
        const gs2sam_drum_note_rule_t *r = &s->cfg.drum_note_rules[i];
        if (r->src_program == program && r->src_note == note) return r;
    }
    return NULL;
}

/* Returns 1 when a target note exists, 0 when the profile intentionally
 * suppresses this source note. */
static int map_drum_note(const gs2sam_t *s, uint8_t src_program,
                         uint8_t src_note, uint8_t *dst_note, int *remapped)
{
    const gs2sam_drum_note_rule_t *nr;
    const gs2sam_drum_kit_rule_t *kr;

    *dst_note = src_note;
    if (remapped) *remapped = 0;
    if (!s->cfg.drum_retarget) return 1;

    nr = find_drum_note_rule(s, src_program, src_note);
    if (nr) {
        if (nr->dst_note == GS2SAM_DRUM_NOTE_DROP) return 0;
        *dst_note = nr->dst_note;
        if (remapped && nr->dst_note != src_note) *remapped = 1;
        return 1;
    }

    kr = find_drum_kit_rule(s, src_program);
    if (kr && (kr->flags & GS2SAM_DRUM_KIT_DROP_UNMAPPED)) return 0;
    return 1;
}

static void reset_drum_map_overrides(gs2sam_t *s, uint8_t map_index)
{
    if (map_index > 1) return;
    memset(s->drum_rx_note_on[map_index], GS2SAM_DRUM_RX_INHERIT, 128);
    memset(s->drum_rx_note_off[map_index], GS2SAM_DRUM_RX_INHERIT, 128);
}

/* GS Key Range is per part, while MIDI note events are per channel.
 * Filtering is exact when all parts sharing a channel have the same range. */
static int channel_key_allows(const gs2sam_t *s, uint8_t ch, uint8_t note,
                              int *conflict)
{
    unsigned p;
    int found = 0;
    uint8_t low = 0, high = 127;

    *conflict = 0;
    for (p = 0; p < 16; ++p) {
        if (s->part_channel[p] != ch) continue;
        if (!found) {
            low = s->part_key_low[p];
            high = s->part_key_high[p];
            found = 1;
        } else if (low != s->part_key_low[p] || high != s->part_key_high[p]) {
            *conflict = 1;
            return 1;
        }
    }
    if (!found) return 1;
    return note >= low && note <= high;
}

static void process_note_message(gs2sam_t *s, uint8_t status,
                                 const uint8_t *d, uint8_t n)
{
    uint8_t type = MIDI_CH_STATUS(status);
    uint8_t ch = MIDI_CH(status);
    uint8_t note = d[0];
    uint8_t out_note = note;
    uint8_t rmap;
    uint8_t src_program = 0;
    int map_conflict = 0;
    int key_conflict = 0;
    int remapped = 0;
    int is_note_on = (type == 0x90u && n == 2 && d[1] != 0);
    int is_note_off = (type == 0x80u && n == 2) ||
                      (type == 0x90u && n == 2 && d[1] == 0);

    if (type != 0x80u && type != 0x90u && type != 0xA0u) return;
    rmap = channel_rhythm_map(s, ch, &map_conflict);
    if (map_conflict) s->stats.approximated++;
    if (rmap >= 1 && rmap <= 2) src_program = s->rhythm_map_program[rmap - 1];

    /* A Note Off for a previously admitted note bypasses later key-range
     * changes. For retargeted drums, use the target note chosen at Note On. */
    if (is_note_off && s->active_note_count[ch][note] != 0) {
        if (rmap && s->cfg.emulate_drum_rx &&
            s->drum_rx_note_off[rmap - 1][note] == 0) {
            s->active_note_count[ch][note]--;
            if (s->active_note_count[ch][note] == 0)
                s->active_note_target[ch][note] = GS2SAM_DRUM_NOTE_DROP;
            s->stats.drum_rx_filtered++;
            s->stats.exact_translate++;
            return;
        }
        if (s->active_note_target[ch][note] != GS2SAM_DRUM_NOTE_DROP)
            out_note = s->active_note_target[ch][note];
        s->active_note_count[ch][note]--;
        if (s->active_note_count[ch][note] == 0)
            s->active_note_target[ch][note] = GS2SAM_DRUM_NOTE_DROP;
        remapped = (out_note != note);
    } else {
        if (s->cfg.emulate_key_range) {
            if (!channel_key_allows(s, ch, note, &key_conflict)) {
                s->stats.key_range_filtered++;
                s->stats.exact_translate++;
                return;
            }
            if (key_conflict) {
                s->stats.key_range_conflicts++;
                s->stats.approximated++;
            }
        }

        if (rmap && s->cfg.emulate_drum_rx) {
            uint8_t rx = is_note_off ? s->drum_rx_note_off[rmap - 1][note]
                                     : s->drum_rx_note_on[rmap - 1][note];
            if (rx == 0) {
                s->stats.drum_rx_filtered++;
                s->stats.exact_translate++;
                return;
            }
        }

        if (rmap) {
            if (!map_drum_note(s, src_program, note, &out_note, &remapped)) {
                s->stats.drum_note_drops++;
                s->stats.approximated++;
                return;
            }
        }

        if (is_note_on) {
            if (s->active_note_count[ch][note] == 0) {
                s->active_note_target[ch][note] = out_note;
            } else if (s->active_note_target[ch][note] != out_note) {
                /* A kit changed while repeated instances of this source note
                 * are outstanding. Drum note-offs are rarely significant, but
                 * flag the ambiguity instead of pretending it is exact. */
                s->stats.drum_note_mapping_conflicts++;
                s->stats.approximated++;
                s->active_note_target[ch][note] = out_note;
            }
            if (s->active_note_count[ch][note] != 0xFFu)
                s->active_note_count[ch][note]++;
        }
    }

    /* Apply rhythm-part mix trim before the first admitted drum attack.
     * This also covers files that never send an explicit CC#7 / Part Level. */
    if (is_note_on && rmap) ensure_part_volume(s, ch);

    if (n == 2) emit3(s, status, out_note, d[1]);
    else emit2(s, status, out_note);

    if (remapped) {
        s->stats.drum_note_remaps++;
        s->stats.approximated++;
    } else {
        s->stats.native_pass++;
    }
}

static const gs2sam_tone_rule_t *find_tone_rule(const gs2sam_t *s,
                                                 uint8_t bank_msb,
                                                 uint8_t bank_lsb,
                                                 uint8_t program)
{
    size_t i;
    for (i = 0; i < s->cfg.tone_rule_count; ++i) {
        const gs2sam_tone_rule_t *r = &s->cfg.tone_rules[i];
        if (r->src_bank_msb == bank_msb && r->src_program == program &&
            (r->src_bank_lsb == GS2SAM_BANK_LSB_ANY || r->src_bank_lsb == bank_lsb)) return r;
    }
    return NULL;
}

static const gs2sam_tone_family_rule_t *find_tone_family_rule(const gs2sam_t *s,
                                                               uint8_t program)
{
    size_t i;
    for (i = 0; i < s->cfg.tone_family_rule_count; ++i) {
        const gs2sam_tone_family_rule_t *r = &s->cfg.tone_family_rules[i];
        if (r->src_program == program) return r;
    }
    return NULL;
}

static void emit_tone_family_rule(gs2sam_t *s, uint8_t ch,
                                  const gs2sam_tone_family_rule_t *r)
{
    if (s->target_bank_msb[ch] != r->dst_bank_msb)
        emit_cc(s, ch, 0, r->dst_bank_msb);
    emit_pc(s, ch, r->dst_program);
    s->target_bank_msb[ch] = r->dst_bank_msb;
}

static void emit_tone_rule(gs2sam_t *s, uint8_t ch, const gs2sam_tone_rule_t *r)
{
    uint8_t f = r->flags;
    emit_cc(s, ch, 0, r->dst_bank_msb);
    emit_pc(s, ch, r->dst_program);
    s->target_bank_msb[ch] = r->dst_bank_msb;

    if (f & GS2SAM_TONE_VIB_RATE)  emit_nrpn(s, ch, 0x01, 0x08, clamp127(64 + r->vibrato_rate));
    if (f & GS2SAM_TONE_VIB_DEPTH) emit_nrpn(s, ch, 0x01, 0x09, clamp127(64 + r->vibrato_depth));
    if (f & GS2SAM_TONE_VIB_DELAY) emit_nrpn(s, ch, 0x01, 0x0A, clamp127(64 + r->vibrato_delay));
    if (f & GS2SAM_TONE_CUTOFF)    emit_nrpn(s, ch, 0x01, 0x20, clamp127(64 + r->cutoff));
    if (f & GS2SAM_TONE_RESONANCE) emit_nrpn(s, ch, 0x01, 0x21, clamp127(64 + r->resonance));
    if (f & GS2SAM_TONE_ATTACK)    emit_nrpn(s, ch, 0x01, 0x63, clamp127(64 + r->attack));
    if (f & GS2SAM_TONE_DECAY)     emit_nrpn(s, ch, 0x01, 0x64, clamp127(64 + r->decay));
    if (f & GS2SAM_TONE_RELEASE)   emit_nrpn(s, ch, 0x01, 0x66, clamp127(64 + r->release));
}

static uint8_t sam_native_drum_program(uint8_t pc)
{
    /* SAM datasheet programs are 1-based: 1,17,41,49,128. */
    return (pc == 0 || pc == 16 || pc == 40 || pc == 48 || pc == 127);
}

static uint8_t drum_target_program(const gs2sam_t *s, uint8_t src_pc,
                                   int *retargeted, int *fallback)
{
    const gs2sam_drum_kit_rule_t *r = NULL;
    if (retargeted) *retargeted = 0;
    if (fallback) *fallback = 0;

    if (s->cfg.drum_retarget) r = find_drum_kit_rule(s, src_pc);
    if (r) {
        if (retargeted && r->dst_program != src_pc) *retargeted = 1;
        return r->dst_program;
    }
    if (sam_native_drum_program(src_pc) || !s->cfg.drum_fallback) return src_pc;
    if (retargeted) *retargeted = 1;
    if (fallback) *fallback = 1;
    return 0;
}

static unsigned emit_drum_program_for_map(gs2sam_t *s, uint8_t rmap,
                                          uint8_t src_pc, uint8_t origin_ch)
{
    uint8_t dst_pc;
    uint16_t seen = 0;
    unsigned p, emitted = 0;
    int retargeted = 0, fallback = 0;

    dst_pc = drum_target_program(s, src_pc, &retargeted, &fallback);

    if (s->cfg.emulate_shared_drum_maps && rmap >= 1 && rmap <= 2) {
        for (p = 0; p < 16; ++p) {
            uint8_t ch;
            uint16_t bit;
            if (s->part_rhythm_map[p] != rmap) continue;
            ch = s->part_channel[p];
            if (ch >= 16) continue;
            bit = (uint16_t)(1u << ch);
            if (seen & bit) continue;
            seen = (uint16_t)(seen | bit);
            emit_pc(s, ch, dst_pc);
            emitted++;
        }
    }

    if (emitted == 0) {
        emit_pc(s, origin_ch, dst_pc);
        emitted = 1;
    }

    if (emitted > 1) s->stats.drum_map_fanouts += emitted - 1;
    if (retargeted) {
        s->stats.approximated++;
        s->stats.drum_kit_retargets++;
    } else {
        s->stats.native_pass++;
    }
    if (fallback) s->stats.drum_fallbacks++;
    return emitted;
}

static void sync_rhythm_channel(gs2sam_t *s, uint8_t ch, uint8_t rmap)
{
    uint8_t src_pc, dst_pc;
    int retargeted = 0, fallback = 0;
    if (ch >= 16 || rmap < 1 || rmap > 2) return;
    src_pc = s->rhythm_map_program[rmap - 1];
    dst_pc = drum_target_program(s, src_pc, &retargeted, &fallback);
    emit_pc(s, ch, dst_pc);
    s->stats.drum_map_syncs++;
    if (retargeted) s->stats.approximated++;
    else s->stats.exact_translate++;
    if (fallback) s->stats.drum_fallbacks++;
}

static void process_program_change(gs2sam_t *s, uint8_t ch, uint8_t pc)
{
    uint8_t bank = s->bank_msb[ch];
    int rmap_conflict = 0;
    uint8_t rmap = channel_rhythm_map(s, ch, &rmap_conflict);

    if (rmap) {
        if (rmap_conflict) s->stats.approximated++;
        if (rmap <= 2) {
            s->rhythm_map_program[rmap - 1] = pc;
            /* Roland documents that Drum Setup Parameters are initialized
             * whenever the Drum Set changes. Keep software overrides aligned. */
            reset_drum_map_overrides(s, (uint8_t)(rmap - 1));
        }
        (void)emit_drum_program_for_map(s, rmap, pc, ch);
        return;
    }

    /* A source profile may override even a target-native bank (notably the
     * SC-55mkII MT-32 bank 127, whose naming/patch layout is not guaranteed
     * byte-for-byte identical to the SAM2695 bank 127). */
    {
        const gs2sam_tone_rule_t *r = find_tone_rule(s, bank, s->bank_lsb[ch], pc);
        if (r) {
            emit_tone_rule(s, ch, r);
            s->stats.approximated++;
            return;
        }
    }

    if (bank == 0 || bank == 127) {
        emit_pc(s, ch, pc);
        s->stats.native_pass++;
        return;
    }

    /* V5 semantic family fallback: exact source-bank mappings remain first
     * priority. For an undocumented/unsupported GS bank, a profile may name a
     * known SAM2695 Bank-127 family patch before we collapse all the way to the
     * GM capital tone. */
    {
        const gs2sam_tone_family_rule_t *fr = find_tone_family_rule(s, pc);
        if (fr) {
            emit_tone_family_rule(s, ch, fr);
            s->stats.approximated++;
            s->stats.tone_family_fallbacks++;
            return;
        }
    }

    if (s->cfg.capital_tone_fallback) {
        if (s->target_bank_msb[ch] != 0) emit_cc(s, ch, 0, 0);
        emit_pc(s, ch, pc);
        s->target_bank_msb[ch] = 0;
        s->stats.approximated++;
        s->stats.tone_fallbacks++;
    } else {
        emit_pc(s, ch, pc);
        s->stats.unsupported++;
    }
}

static void process_channel(gs2sam_t *s, uint8_t status, const uint8_t *d, uint8_t n)
{
    uint8_t type = MIDI_CH_STATUS(status);
    uint8_t ch = MIDI_CH(status);
    s->stats.channel_messages++;

    if ((type == 0x80u || type == 0x90u || type == 0xA0u) && n == 2) {
        process_note_message(s, status, d, n);
        return;
    }

    if (type == 0xB0u && n == 2) {
        uint8_t cc = d[0], v = d[1];
        if (cc == 7) {
            emit_part_volume(s, ch, v, 1);
            if (channel_is_rhythm(s, ch) && s->cfg.rhythm_volume_percent != 100)
                s->stats.approximated++;
            else
                s->stats.native_pass++;
            return;
        }
        if (cc == 0) {
            s->bank_msb[ch] = v;
            if (v == 0 || v == 127 || channel_is_rhythm(s, ch)) {
                emit3(s, status, cc, v);
                s->target_bank_msb[ch] = v;
                s->stats.native_pass++;
            } else {
                /* Hold unsupported GS variation bank until Program Change. */
            }
            return;
        }
        if (cc == 32) s->bank_lsb[ch] = v;
        emit3(s, status, cc, v);
        s->stats.native_pass++;
        return;
    }

    if (type == 0xC0u && n == 1) {
        process_program_change(s, ch, d[0]);
        return;
    }

    if (n == 1) emit2(s, status, d[0]);
    else emit3(s, status, d[0], d[1]);
    s->stats.native_pass++;
}

static int roland_dt1(const uint8_t *m, size_t n,
                      uint8_t *a0, uint8_t *a1, uint8_t *a2,
                      const uint8_t **data, size_t *data_len)
{
    size_t payload;
    uint8_t expected;

    if (n < 11) return 0;
    if (m[0] != 0xF0 || m[n - 1] != 0xF7) return 0;
    if (m[1] != 0x41 || m[3] != 0x42 || m[4] != 0x12) return 0;

    *a0 = m[5]; *a1 = m[6]; *a2 = m[7];
    payload = n - 10; /* bytes after 3-byte address, before checksum/F7 */
    *data = &m[8];
    *data_len = payload;
    expected = gs2sam_roland_checksum(&m[5], 3 + payload);
    return expected == m[n - 2] ? 1 : -1;
}

static int sam_native_gs_address(uint8_t a0, uint8_t a1, uint8_t a2, size_t len)
{
    uint8_t lo = (uint8_t)(a1 & 0x0Fu);

    if (a0 != 0x40) return 0;
    if (a1 == 0x00) {
        if (a2 == 0x00 && len == 4) return 1; /* master tune */
        if ((a2 == 0x04 || a2 == 0x05 || a2 == 0x06 || a2 == 0x7F) && len == 1) return 1;
    }
    if (a1 == 0x01) {
        if (a2 == 0x10 && len == 16) return 1; /* voice reserve */
        if ((a2 == 0x30 || a2 == 0x31 || a2 == 0x33 || a2 == 0x34 || a2 == 0x35 ||
             a2 == 0x38 || a2 == 0x3A || a2 == 0x3B || a2 == 0x3C || a2 == 0x3D || a2 == 0x3E) && len == 1) return 1;
    }
    if ((a1 & 0xF0u) == 0x10u) {
        (void)lo;
        if (a2 == 0x02 && len == 1) return 1; /* part channel */
        if (a2 == 0x15 && len == 1) return 1; /* rhythm MAP1 only; MAP2 rewritten */
        if (a2 == 0x40 && len == 12) return 1; /* scale tune */
        if ((a2 == 0x1A || a2 == 0x1B || a2 == 0x1F || a2 == 0x20) && len == 1) return 1;
    }
    if ((a1 & 0xF0u) == 0x20u && len == 1) {
        switch (a2) {
            case 0x00: case 0x01: case 0x02: case 0x03: case 0x04: case 0x05: case 0x06:
            case 0x10: case 0x11: case 0x12: case 0x14: case 0x15: case 0x16:
            case 0x20: case 0x21: case 0x22: case 0x24: case 0x25: case 0x26:
            case 0x40: case 0x41: case 0x42: case 0x44: case 0x45: case 0x46:
            case 0x50: case 0x51: case 0x52: case 0x54: case 0x55: case 0x56:
                return 1;
            default: break;
        }
    }
    return 0;
}

static unsigned channel_part_count(const gs2sam_t *s, uint8_t ch)
{
    unsigned p, n = 0;
    for (p = 0; p < 16; ++p) if (s->part_channel[p] == ch) ++n;
    return n;
}

static void rewrite_roland_one_byte(gs2sam_t *s, const uint8_t *m, size_t n, uint8_t value)
{
    uint8_t out[16];
    size_t k;
    if (n > sizeof(out)) {
        emit_raw(s, m, n);
        return;
    }
    memcpy(out, m, n);
    out[8] = value;
    out[n - 2] = gs2sam_roland_checksum(&out[5], n - 7);
    for (k = 0; k < n; ++k) (void)out[k];
    emit_raw(s, out, n);
}

static int translate_drum_setup(gs2sam_t *s,
                                uint8_t a0, uint8_t a1, uint8_t note,
                                const uint8_t *data, size_t len)
{
    uint8_t map, kind, nrpn_msb;
    uint8_t target_note = note;
    unsigned p;
    uint16_t seen = 0;
    int emitted = 0;
    int remapped = 0;

    if (a0 != 0x41 || len != 1) return 0;
    map = (uint8_t)((a1 >> 4) & 0x0Fu);
    kind = (uint8_t)(a1 & 0x0Fu);
    if (map > 1) return 0;

    /* Explicit Rx Note On/Off can be emulated exactly in the translator.
     * 0xFF remains the inherited/native target-kit behavior until explicitly
     * written by GS SysEx. */
    if (kind == 7 || kind == 8) {
        uint8_t v = data[0] ? 1u : 0u;
        if (kind == 7) s->drum_rx_note_off[map][note] = v;
        else s->drum_rx_note_on[map][note] = v;
        s->stats.drum_rx_updates++;
        s->stats.exact_translate++;
        return 1;
    }

    /* 41 m1 rr is GS PLAY NOTE NUMBER / absolute Pitch coarse. Roland's NRPN
     * 18rr is a relative offset around 40h. Converting this correctly requires
     * the selected source kit's default PLAY NOTE NUMBER table; R3 still does
     * not guess those defaults. Preserve the SysEx for forward compatibility. */
    if (kind == 1) return 0;

    switch (kind) {
        case 2: nrpn_msb = 0x1A; break; /* level */
        case 4: nrpn_msb = 0x1C; break; /* pan */
        case 5: nrpn_msb = 0x1D; break; /* reverb */
        case 6: nrpn_msb = 0x1E; break; /* chorus */
        default: return 0;
    }

    if (!map_drum_note(s, s->rhythm_map_program[map], note,
                       &target_note, &remapped)) {
        s->stats.drum_setup_note_drops++;
        s->stats.approximated++;
        return 1;
    }

    for (p = 0; p < 16; ++p) {
        uint8_t ch;
        uint16_t bit;
        if (s->part_rhythm_map[p] != (uint8_t)(map + 1)) continue;
        ch = s->part_channel[p];
        if (ch >= 16) continue;
        bit = (uint16_t)(1u << ch);
        if (seen & bit) continue;
        seen = (uint16_t)(seen | bit);
        emit_nrpn(s, ch, nrpn_msb, target_note, data[0]);
        emitted = 1;
    }
    if (emitted) {
        if (remapped) {
            s->stats.drum_setup_note_remaps++;
            s->stats.approximated++;
        } else {
            s->stats.exact_translate++;
        }
    } else {
        s->stats.unsupported++;
    }
    return 1;
}

static void process_sysex(gs2sam_t *s, const uint8_t *m, size_t n)
{
    uint8_t a0, a1, a2;
    const uint8_t *data;
    size_t len;
    int r;

    s->stats.sysex_messages++;

    /* Universal General MIDI System On. Keep translator shadow state in step
     * with the target. */
    if (n == 6 && m[0] == 0xF0 && m[1] == 0x7E && m[3] == 0x09 &&
        m[4] == 0x01 && m[5] == 0xF7) {
        emit_raw(s, m, n);
        reset_gs_state(s);
        s->stats.native_pass++;
        return;
    }

    r = roland_dt1(m, n, &a0, &a1, &a2, &data, &len);
    if (r == -1) {
        s->stats.malformed_sysex++;
        if (s->cfg.pass_malformed_sysex) emit_raw(s, m, n);
        return;
    }
    if (r == 0) {
        if (s->cfg.pass_unknown_sysex) emit_raw(s, m, n);
        s->stats.unsupported++;
        return;
    }

    /* GS Reset: pass natively and reset translator state. */
    if (a0 == 0x40 && a1 == 0x00 && a2 == 0x7F && len == 1 && data[0] == 0x00) {
        emit_raw(s, m, n);
        reset_gs_state(s);
        s->stats.native_pass++;
        return;
    }

    /* GS TONE NUMBER is a two-byte part parameter: bank (CC#0 value),
     * then program number.  SAM2695 has no equivalent part-addressed SysEx,
     * so feed it through the same bank/program retargeter used for channel MIDI. */
    if (a0 == 0x40 && (a1 & 0xF0u) == 0x10u && a2 == 0x00 && len == 2) {
        uint8_t p = (uint8_t)(a1 & 0x0Fu);
        uint8_t ch = s->part_channel[p];
        if (ch < 16) {
            s->bank_msb[ch] = data[0];
            /* Channel Bank Select is absent from this part-addressed SysEx.
             * For target-native banks, explicitly establish the target bank
             * before Program Change.  Rules/fallback establish theirs inside
             * process_program_change(). */
            if ((data[0] == 0 || data[0] == 127) &&
                s->target_bank_msb[ch] != data[0]) {
                emit_cc(s, ch, 0, data[0]);
                s->target_bank_msb[ch] = data[0];
            }
            process_program_change(s, ch, data[1]);
            s->stats.tone_sysex_translates++;
        } else {
            s->stats.unsupported++;
        }
        return;
    }

    /* Part block parameters. p/block is low nibble of 1p. */
    if (a0 == 0x40 && (a1 & 0xF0u) == 0x10u && len == 1) {
        uint8_t p = (uint8_t)(a1 & 0x0Fu);
        uint8_t ch = s->part_channel[p];

        if (a2 == 0x02) { /* MIDI channel to part assign */
            uint8_t old_ch = s->part_channel[p];
            s->part_channel[p] = data[0] <= 16 ? data[0] : 16;
            emit_raw(s, m, n);
            s->stats.native_pass++;
            /* The rhythm/melodic role of both channels may have changed.
             * Restore/reapply the source CC#7 using the new role. */
            if (s->cfg.rhythm_volume_percent != 100u && old_ch < 16)
                emit_part_volume(s, old_ch, s->source_volume[old_ch], 1);
            ch = s->part_channel[p];
            if (ch < 16) {
                if (s->part_rhythm_map[p] >= 1 && s->part_rhythm_map[p] <= 2)
                    sync_rhythm_channel(s, ch, s->part_rhythm_map[p]);
                if (s->cfg.rhythm_volume_percent != 100u)
                    emit_part_volume(s, ch, s->source_volume[ch], 1);
            }
            return;
        }
        if (a2 == 0x15) { /* USE FOR RHYTHM PART */
            s->part_rhythm_map[p] = data[0] <= 2 ? data[0] : 0;
            if (data[0] == 2) {
                /* SAM has only sound/rhythm flag. Preserve source MAP2 in our
                 * state, but tell SAM to make this a rhythm part (MAP1). */
                rewrite_roland_one_byte(s, m, n, 1);
                s->stats.approximated++;
                s->stats.map2_collapses++;
            } else {
                emit_raw(s, m, n);
                s->stats.native_pass++;
            }
            /* A newly-created rhythm part must inherit the current logical
             * Drum1/Drum2 kit. SAM rhythm allocation is only a boolean flag,
             * so establish the retargeted kit explicitly on its MIDI channel. */
            if (data[0] >= 1 && data[0] <= 2 && ch < 16)
                sync_rhythm_channel(s, ch, data[0]);
            if (s->cfg.rhythm_volume_percent != 100u && ch < 16)
                emit_part_volume(s, ch, s->source_volume[ch], 1);
            return;
        }

        if (a2 == 0x1D) { /* KEY RANGE LOW: software emulation */
            s->part_key_low[p] = data[0];
            s->stats.exact_translate++;
            s->stats.key_range_updates++;
            return;
        }
        if (a2 == 0x1E) { /* KEY RANGE HIGH: software emulation */
            s->part_key_high[p] = data[0];
            s->stats.exact_translate++;
            s->stats.key_range_updates++;
            return;
        }

        if (ch < 16 && a2 >= 0x30 && a2 <= 0x37) {
            static const uint8_t nrpn_lsb[8] = {
                0x08, 0x09, 0x20, 0x21, 0x63, 0x64, 0x66, 0x0A
            };
            emit_nrpn(s, ch, 0x01, nrpn_lsb[a2 - 0x30], data[0]);
            s->stats.exact_translate++;
            s->stats.tone_modify_translates++;
            return;
        }

        if (ch < 16) {
            if (a2 == 0x19) { /* PART LEVEL = CC#7 */
                emit_part_volume(s, ch, data[0], 1);
                s->stats.exact_translate++;
                if (channel_is_rhythm(s, ch) && s->cfg.rhythm_volume_percent != 100)
                    s->stats.approximated++;
                if (channel_part_count(s, ch) > 1) s->stats.approximated++;
                return;
            }
            if (a2 == 0x1C) { /* PART PAN = CC#10 except random(0) */
                if (data[0] == 0) {
                    emit_cc(s, ch, 10, 64);
                    s->stats.approximated++;
                } else {
                    emit_cc(s, ch, 10, data[0]);
                    s->stats.exact_translate++;
                }
                return;
            }
            if (a2 == 0x21) { /* CHORUS SEND = CC#93 */
                emit_cc(s, ch, 93, data[0]);
                s->stats.exact_translate++;
                return;
            }
            if (a2 == 0x22) { /* REVERB SEND = CC#91 */
                emit_cc(s, ch, 91, data[0]);
                s->stats.exact_translate++;
                return;
            }
            if (a2 == 0x16) { /* PITCH KEY SHIFT -> RPN 0002 coarse tune */
                emit_rpn(s, ch, 0, 2, data[0]);
                s->stats.exact_translate++;
                return;
            }
        }
    }

    /* SC-55 drum setup: translate exact parameters to SAM per-channel NRPN.
     * 41 m2 rr level, m4 pan, m5 reverb, m6 chorus. */
    if (translate_drum_setup(s, a0, a1, a2, data, len)) return;

    if (sam_native_gs_address(a0, a1, a2, len)) {
        emit_raw(s, m, n);
        s->stats.native_pass++;
        return;
    }

    if (s->cfg.pass_unknown_sysex) emit_raw(s, m, n);
    s->stats.unsupported++;
}

static uint8_t channel_data_need(uint8_t status)
{
    switch (status & 0xF0u) {
        case 0xC0: case 0xD0: return 1;
        case 0x80: case 0x90: case 0xA0: case 0xB0: case 0xE0: return 2;
        default: return 0;
    }
}

static void flush_incomplete_sysex(gs2sam_t *s)
{
    if (s->sysex_len) emit_raw(s, s->sysex, s->sysex_len);
    s->sysex_len = 0;
    s->in_sysex = 0;
    s->sysex_passthrough = 0;
}

void gs2sam_feed(gs2sam_t *s, uint8_t b)
{
    s->stats.bytes_in++;

    /* Realtime can occur anywhere, including inside SysEx, and normally does
     * not alter running status. System Reset (FF) also resets our shadow GS
     * state because the SAM2695 returns to its power-up condition. */
    if (b >= 0xF8u) {
        emit1(s, b);
        if (b == 0xFFu) reset_gs_state(s);
        return;
    }

    if (s->in_sysex) {
        if (s->sysex_passthrough) {
            emit1(s, b);
            if (b == 0xF7u) {
                s->in_sysex = 0;
                s->sysex_passthrough = 0;
                s->sysex_len = 0;
            }
            return;
        }

        if (b == 0xF7u) {
            if (s->sysex_len < GS2SAM_SYSEX_MAX) s->sysex[s->sysex_len++] = b;
            process_sysex(s, s->sysex, s->sysex_len);
            s->in_sysex = 0;
            s->sysex_len = 0;
            s->running_status = 0;
            return;
        }

        if ((b & 0x80u) && b != 0xF0u) {
            /* Broken SysEx: preserve bytes, then parse the new status. */
            s->stats.malformed_sysex++;
            flush_incomplete_sysex(s);
            /* fall through to normal status handling */
        } else {
            if (s->sysex_len < GS2SAM_SYSEX_MAX) {
                s->sysex[s->sysex_len++] = b;
            } else {
                /* Long manufacturer dump: stop buffering and stream it. */
                emit_raw(s, s->sysex, s->sysex_len);
                s->sysex_len = 0;
                s->sysex_passthrough = 1;
                emit1(s, b);
            }
            return;
        }
    }

    if (b & 0x80u) {
        s->msg_have = 0;
        s->system_common = 0;

        if (b == 0xF0u) {
            s->running_status = 0;
            s->in_sysex = 1;
            s->sysex_len = 0;
            s->sysex_passthrough = 0;
            s->sysex[s->sysex_len++] = b;
            return;
        }

        if (b < 0xF0u) {
            s->running_status = b;
            s->msg_status = b;
            s->msg_need = channel_data_need(b);
            return;
        }

        /* System common cancels running status. */
        s->running_status = 0;
        s->msg_status = b;
        s->system_common = 1;
        switch (b) {
            case 0xF1: s->msg_need = 1; return;
            case 0xF2: s->msg_need = 2; return;
            case 0xF3: s->msg_need = 1; return;
            case 0xF6: case 0xF7: case 0xF4: case 0xF5:
            default:
                emit1(s, b);
                s->system_common = 0;
                s->msg_need = 0;
                return;
        }
    }

    /* Data byte. */
    if (s->system_common) {
        if (s->msg_have < 2) s->msg_data[s->msg_have++] = b;
        if (s->msg_have == s->msg_need) {
            if (s->msg_need == 1) emit2(s, s->msg_status, s->msg_data[0]);
            else emit3(s, s->msg_status, s->msg_data[0], s->msg_data[1]);
            s->system_common = 0;
            s->msg_have = 0;
        }
        return;
    }

    if (s->running_status) {
        s->msg_status = s->running_status;
        s->msg_need = channel_data_need(s->msg_status);
        if (s->msg_have < 2) s->msg_data[s->msg_have++] = b;
        if (s->msg_have == s->msg_need) {
            process_channel(s, s->msg_status, s->msg_data, s->msg_need);
            s->msg_have = 0;
        }
        return;
    }

    /* Stray data byte: preserve rather than drop. */
    emit1(s, b);
}

void gs2sam_feed_buffer(gs2sam_t *s, const uint8_t *bytes, size_t len)
{
    size_t i;
    for (i = 0; i < len; ++i) gs2sam_feed(s, bytes[i]);
}
