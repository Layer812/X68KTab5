#include "gs2sam_processor.h"
#include "gs2sam_profile_sc55mk2.h"

#include <string.h>

static void proc_reset(void *user)
{
    gs2sam_processor_t *p = (gs2sam_processor_t *)user;
    if (p != 0) gs2sam_reset(&p->core);
}

static void proc_feed(void *user, uint8_t byte)
{
    gs2sam_processor_t *p = (gs2sam_processor_t *)user;
    if (p != 0) gs2sam_feed(&p->core, byte);
}

static void install_config(gs2sam_processor_t *p, const gs2sam_config_t *config)
{
    if (p == 0 || p->engine == 0 || config == 0) return;
    p->config = *config;
    p->config.emit = midi_engine_output;
    p->config.emit_user = p->engine;
    gs2sam_init(&p->core, &p->config);
}

void gs2sam_processor_init_sc55mk2(gs2sam_processor_t *p,
                                   midi_engine_t *engine,
                                   uint8_t rhythm_volume_percent)
{
    if (p == 0 || engine == 0) return;
    memset(p, 0, sizeof(*p));
    p->engine = engine;
    gs2sam_config_t cfg;
    gs2sam_sc55mk2_config(&cfg, midi_engine_output, engine);
    cfg.rhythm_volume_percent = rhythm_volume_percent;
    install_config(p, &cfg);
}

void gs2sam_processor_apply_sc55mk2(gs2sam_processor_t *p,
                                    uint8_t rhythm_volume_percent)
{
    if (p == 0 || p->engine == 0) return;
    gs2sam_config_t cfg;
    gs2sam_sc55mk2_config(&cfg, midi_engine_output, p->engine);
    cfg.rhythm_volume_percent = rhythm_volume_percent;
    install_config(p, &cfg);
}

void gs2sam_processor_apply_config(gs2sam_processor_t *p,
                                   const gs2sam_config_t *config)
{
    install_config(p, config);
}


midi_processor_t gs2sam_processor_interface(gs2sam_processor_t *p)
{
    midi_processor_t iface;
    iface.reset = proc_reset;
    iface.feed_byte = proc_feed;
    iface.user = p;
    return iface;
}

const gs2sam_stats_t *gs2sam_processor_stats(const gs2sam_processor_t *p)
{
    return p != 0 ? gs2sam_get_stats(&p->core) : 0;
}
