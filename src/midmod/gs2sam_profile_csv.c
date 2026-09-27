#include "gs2sam_profile_csv.h"

#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define FIELD_MAX 20u

static char *trim(char *s)
{
    char *end;
    while (*s != '\0' && isspace((unsigned char)*s)) ++s;
    end = s + strlen(s);
    while (end > s && isspace((unsigned char)end[-1])) --end;
    *end = '\0';
    return s;
}

static size_t split_csv_simple(char *line, char **fields, size_t max_fields)
{
    size_t n = 0;
    char *p = line;
    if (max_fields == 0u) return 0u;
    fields[n++] = p;
    while (*p != '\0') {
        if (*p == ',') {
            *p = '\0';
            if (n < max_fields) fields[n++] = p + 1;
        }
        ++p;
    }
    return n;
}

static void set_error(gs2sam_csv_profile_t *p, uint32_t line_no, const char *msg)
{
    ++p->errors;
    (void)snprintf(p->last_error, sizeof(p->last_error),
                   "line %lu: %s", (unsigned long)line_no, msg);
}

static int parse_long_range(const char *s, long lo, long hi, long *out)
{
    char *end = NULL;
    long v;
    if (s == NULL || *s == '\0') return 0;
    errno = 0;
    v = strtol(s, &end, 0);
    if (errno != 0 || end == s || *trim(end) != '\0' || v < lo || v > hi) return 0;
    *out = v;
    return 1;
}

static int parse_bool(const char *s, uint8_t *out)
{
    if (strcmp(s, "1") == 0 || strcmp(s, "true") == 0 || strcmp(s, "on") == 0 || strcmp(s, "yes") == 0) {
        *out = 1u;
        return 1;
    }
    if (strcmp(s, "0") == 0 || strcmp(s, "false") == 0 || strcmp(s, "off") == 0 || strcmp(s, "no") == 0) {
        *out = 0u;
        return 1;
    }
    return 0;
}

static void copy_meta(char *dst, size_t cap, const char *src)
{
    if (cap == 0u) return;
    (void)snprintf(dst, cap, "%s", src != NULL ? src : "");
}

static int parse_meta(gs2sam_csv_profile_t *p, char **f, size_t n)
{
    if (n < 3u) return -1;
    if (strcmp(f[1], "name") == 0) copy_meta(p->name, sizeof(p->name), f[2]);
    else if (strcmp(f[1], "source") == 0) copy_meta(p->source, sizeof(p->source), f[2]);
    else if (strcmp(f[1], "target") == 0) copy_meta(p->target, sizeof(p->target), f[2]);
    else if (strcmp(f[1], "status") == 0) copy_meta(p->status, sizeof(p->status), f[2]);
    else return 0;
    return 1;
}

static int parse_config(gs2sam_csv_profile_t *p, char **f, size_t n)
{
    long v;
    uint8_t b;
    if (n < 3u) return -1;
    if (strcmp(f[1], "rhythm_volume_percent") == 0) {
        if (!parse_long_range(f[2], 0, 200, &v)) return -1;
        p->rhythm_volume_percent = (uint8_t)v;
        return 1;
    }
#define BOOL_CFG(KEY, MEMBER) \
    if (strcmp(f[1], KEY) == 0) { \
        if (!parse_bool(f[2], &b)) return -1; \
        p->MEMBER = b; \
        return 1; \
    }
    BOOL_CFG("pass_unknown_sysex", pass_unknown_sysex)
    BOOL_CFG("pass_malformed_sysex", pass_malformed_sysex)
    BOOL_CFG("capital_tone_fallback", capital_tone_fallback)
    BOOL_CFG("drum_fallback", drum_fallback)
    BOOL_CFG("drum_retarget", drum_retarget)
    BOOL_CFG("emulate_shared_drum_maps", emulate_shared_drum_maps)
    BOOL_CFG("emulate_drum_rx", emulate_drum_rx)
    BOOL_CFG("emulate_key_range", emulate_key_range)
#undef BOOL_CFG
    return 0;
}

static int parse_tone(gs2sam_csv_profile_t *p, char **f, size_t n)
{
    gs2sam_tone_rule_t *r;
    long v;
    size_t i;
    static const uint8_t flags[8] = {
        GS2SAM_TONE_VIB_RATE, GS2SAM_TONE_VIB_DEPTH, GS2SAM_TONE_VIB_DELAY,
        GS2SAM_TONE_CUTOFF, GS2SAM_TONE_RESONANCE, GS2SAM_TONE_ATTACK,
        GS2SAM_TONE_DECAY, GS2SAM_TONE_RELEASE
    };
    int8_t *mods[8];

    if (n < 6u || p->tone_rule_count >= GS2SAM_CSV_MAX_TONE_RULES) return -1;
    r = &p->tone_rules[p->tone_rule_count];
    memset(r, 0, sizeof(*r));
    if (!parse_long_range(f[1], 0, 127, &v)) return -1;
    r->src_bank_msb = (uint8_t)v;
    if (strcmp(f[2], "*") == 0 || f[2][0] == '\0') r->src_bank_lsb = GS2SAM_BANK_LSB_ANY;
    else {
        if (!parse_long_range(f[2], 0, 127, &v)) return -1;
        r->src_bank_lsb = (uint8_t)v;
    }
    if (!parse_long_range(f[3], 1, 128, &v)) return -1;
    r->src_program = (uint8_t)(v - 1);
    if (!parse_long_range(f[4], 0, 127, &v)) return -1;
    r->dst_bank_msb = (uint8_t)v;
    if (!parse_long_range(f[5], 1, 128, &v)) return -1;
    r->dst_program = (uint8_t)(v - 1);

    mods[0] = &r->vibrato_rate;
    mods[1] = &r->vibrato_depth;
    mods[2] = &r->vibrato_delay;
    mods[3] = &r->cutoff;
    mods[4] = &r->resonance;
    mods[5] = &r->attack;
    mods[6] = &r->decay;
    mods[7] = &r->release;
    for (i = 0; i < 8u; ++i) {
        size_t field = 6u + i;
        if (field >= n || f[field][0] == '\0') continue;
        if (!parse_long_range(f[field], -64, 63, &v)) return -1;
        *mods[i] = (int8_t)v;
        r->flags = (uint8_t)(r->flags | flags[i]);
    }
    ++p->tone_rule_count;
    return 1;
}

static int parse_family(gs2sam_csv_profile_t *p, char **f, size_t n)
{
    gs2sam_tone_family_rule_t *r;
    long v;
    if (n < 4u || p->tone_family_rule_count >= GS2SAM_CSV_MAX_TONE_FAMILY_RULES) return -1;
    r = &p->tone_family_rules[p->tone_family_rule_count];
    memset(r, 0, sizeof(*r));
    if (!parse_long_range(f[1], 1, 128, &v)) return -1;
    r->src_program = (uint8_t)(v - 1);
    if (!parse_long_range(f[2], 0, 127, &v)) return -1;
    r->dst_bank_msb = (uint8_t)v;
    if (!parse_long_range(f[3], 1, 128, &v)) return -1;
    r->dst_program = (uint8_t)(v - 1);
    ++p->tone_family_rule_count;
    return 1;
}

static int parse_drumkit(gs2sam_csv_profile_t *p, char **f, size_t n)
{
    gs2sam_drum_kit_rule_t *r;
    long v;
    if (n < 3u || p->drum_kit_rule_count >= GS2SAM_CSV_MAX_DRUM_KIT_RULES) return -1;
    r = &p->drum_kit_rules[p->drum_kit_rule_count];
    memset(r, 0, sizeof(*r));
    if (!parse_long_range(f[1], 1, 128, &v)) return -1;
    r->src_program = (uint8_t)(v - 1);
    if (!parse_long_range(f[2], 1, 128, &v)) return -1;
    r->dst_program = (uint8_t)(v - 1);
    if (n >= 4u && f[3][0] != '\0' && strcmp(f[3], "0") != 0) {
        if (strcmp(f[3], "drop_unmapped") == 0) r->flags = GS2SAM_DRUM_KIT_DROP_UNMAPPED;
        else return -1;
    }
    ++p->drum_kit_rule_count;
    return 1;
}

static int parse_drumnote(gs2sam_csv_profile_t *p, char **f, size_t n)
{
    gs2sam_drum_note_rule_t *r;
    long v;
    if (n < 4u || p->drum_note_rule_count >= GS2SAM_CSV_MAX_DRUM_NOTE_RULES) return -1;
    r = &p->drum_note_rules[p->drum_note_rule_count];
    memset(r, 0, sizeof(*r));
    if (!parse_long_range(f[1], 1, 128, &v)) return -1;
    r->src_program = (uint8_t)(v - 1);
    if (!parse_long_range(f[2], 0, 127, &v)) return -1;
    r->src_note = (uint8_t)v;
    if (strcmp(f[3], "drop") == 0) r->dst_note = GS2SAM_DRUM_NOTE_DROP;
    else {
        if (!parse_long_range(f[3], 0, 127, &v)) return -1;
        r->dst_note = (uint8_t)v;
    }
    ++p->drum_note_rule_count;
    return 1;
}

void gs2sam_csv_profile_init(gs2sam_csv_profile_t *p)
{
    memset(p, 0, sizeof(*p));
    copy_meta(p->name, sizeof(p->name), "Unnamed CSV profile");
    copy_meta(p->target, sizeof(p->target), "SAM2695");
    p->pass_unknown_sysex = 1u;
    p->pass_malformed_sysex = 1u;
    p->capital_tone_fallback = 1u;
    p->drum_fallback = 1u;
    p->drum_retarget = 1u;
    p->emulate_shared_drum_maps = 1u;
    p->emulate_drum_rx = 1u;
    p->emulate_key_range = 1u;
    p->rhythm_volume_percent = 100u;
}

int gs2sam_csv_profile_parse_line(gs2sam_csv_profile_t *p,
                                  char *line,
                                  uint32_t line_no)
{
    char *f[FIELD_MAX];
    size_t n, i;
    char *s;
    int rc;
    if (p == NULL || line == NULL) return -1;
    ++p->lines_seen;
    s = trim(line);
    if (*s == '\0') {
        ++p->rows_ignored;
        return 0;
    }
    if (*s == '#') {
        char *comment = trim(s + 1);
        if (line_no == 1u) {
            copy_meta(p->comment, sizeof(p->comment), comment);
        }
        if (strcmp(comment, "GS2SAM_PROFILE,1") == 0) {
            p->signature_seen = 1u;
        }
        ++p->rows_ignored;
        return 0;
    }
    n = split_csv_simple(s, f, FIELD_MAX);
    for (i = 0; i < n; ++i) f[i] = trim(f[i]);

    if (strcmp(f[0], "meta") == 0) rc = parse_meta(p, f, n);
    else if (strcmp(f[0], "config") == 0) rc = parse_config(p, f, n);
    else if (strcmp(f[0], "tone") == 0) rc = parse_tone(p, f, n);
    else if (strcmp(f[0], "family") == 0) rc = parse_family(p, f, n);
    else if (strcmp(f[0], "drumkit") == 0) rc = parse_drumkit(p, f, n);
    else if (strcmp(f[0], "drumnote") == 0) rc = parse_drumnote(p, f, n);
    else {
        ++p->rows_ignored; /* forward-compatible unknown row type */
        return 0;
    }

    if (rc < 0) {
        set_error(p, line_no, "invalid or out-of-range CSV row");
        return -1;
    }
    if (rc == 0) {
        ++p->rows_ignored; /* unknown metadata/config key */
        return 0;
    }
    ++p->rows_loaded;
    return 1;
}

void gs2sam_csv_profile_make_config(const gs2sam_csv_profile_t *p,
                                    gs2sam_config_t *cfg,
                                    gs2sam_emit_fn emit,
                                    void *emit_user)
{
    gs2sam_default_config(cfg, emit, emit_user);
    if (p == NULL) return;
    cfg->pass_unknown_sysex = p->pass_unknown_sysex;
    cfg->pass_malformed_sysex = p->pass_malformed_sysex;
    cfg->capital_tone_fallback = p->capital_tone_fallback;
    cfg->drum_fallback = p->drum_fallback;
    cfg->drum_retarget = p->drum_retarget;
    cfg->emulate_shared_drum_maps = p->emulate_shared_drum_maps;
    cfg->emulate_drum_rx = p->emulate_drum_rx;
    cfg->emulate_key_range = p->emulate_key_range;
    cfg->rhythm_volume_percent = p->rhythm_volume_percent;
    cfg->tone_rules = p->tone_rules;
    cfg->tone_rule_count = p->tone_rule_count;
    cfg->tone_family_rules = p->tone_family_rules;
    cfg->tone_family_rule_count = p->tone_family_rule_count;
    cfg->drum_kit_rules = p->drum_kit_rules;
    cfg->drum_kit_rule_count = p->drum_kit_rule_count;
    cfg->drum_note_rules = p->drum_note_rules;
    cfg->drum_note_rule_count = p->drum_note_rule_count;
}
