#include <stdint.h>
#include <stddef.h>

#define LP_SHARED __attribute__((section(".lp_shared"), aligned(4), used))
#define CMD_LEN 256u
#define CMD_MASK (CMD_LEN - 1u)
#define OUT_LEN 4096u
#define OUT_MASK (OUT_LEN - 1u)
#define RAW_LEN 256u
#define RAW_MASK (RAW_LEN - 1u)
#define READY_MAGIC 0x4C504144u /* 'LPAD' */
#define PROBE_MAGIC 0x48503241u /* 'HP2A' */
#define SAMPLE_RATE_X12 (44100u * 12u)
#define FM_IPSCALE 256L
#define ADPCMMAX 2047
#define ADPCMMIN (-2048)

enum {
    CMD_RESET = 1,
    CMD_CONTROL = 2,
    CMD_DATA = 3,
    CMD_PAN = 4,
    CMD_CLOCK = 5,
    CMD_VOLUME = 6,
    CMD_RENDER = 7
};

LP_SHARED volatile uint32_t lp_adpcm_cmd_tail;
LP_SHARED volatile uint32_t lp_adpcm_cmd_head;
LP_SHARED volatile uint32_t lp_adpcm_out_tail;
LP_SHARED volatile uint32_t lp_adpcm_out_head;
LP_SHARED volatile uint32_t lp_adpcm_out_stalls;
LP_SHARED volatile uint32_t lp_adpcm_raw_overruns;
LP_SHARED volatile uint32_t lp_adpcm_work_frames;
LP_SHARED volatile uint32_t lp_adpcm_work_calls;
LP_SHARED volatile uint32_t lp_adpcm_work_us;
LP_SHARED volatile uint32_t lp_adpcm_heartbeat;
LP_SHARED volatile uint32_t lp_adpcm_hp_probe;
LP_SHARED volatile uint32_t lp_adpcm_lp_echo;
LP_SHARED volatile uint32_t lp_adpcm_diag_stage;
LP_SHARED volatile uint32_t lp_adpcm_diag_w0;
LP_SHARED volatile uint32_t lp_adpcm_diag_w1;
LP_SHARED volatile uint32_t lp_adpcm_diag_mcause;
LP_SHARED volatile uint32_t lp_adpcm_diag_mepc;
LP_SHARED volatile uint32_t lp_adpcm_diag_mtval;
LP_SHARED volatile uint32_t lp_adpcm_ready;
LP_SHARED volatile uint32_t lp_adpcm_cmd_ring[CMD_LEN * 2u];
LP_SHARED volatile uint32_t lp_adpcm_out_ring[OUT_LEN];

static uint32_t s_raw[RAW_LEN];
static uint32_t s_raw_rd, s_raw_wr;
static int32_t s_out;
static int32_t s_step;
static uint32_t s_count;
static uint32_t s_clock_rate;
static uint8_t s_clock;
static uint8_t s_pan;
static uint8_t s_playing;
static uint8_t s_volume;
static int32_t s_old_r, s_old_l;
static int32_t s_outs[8];
static int32_t s_outsip[4];
static int32_t s_outsip_r[4];
static int32_t s_outsip_l[4];

static const int8_t s_index_shift[8] = {-1,-1,-1,-1,2,4,6,8};
static const uint16_t s_base_step[49] = {
    16,17,19,21,23,25,28,31,34,37,41,45,50,55,60,66,73,80,88,97,
    107,118,130,143,157,173,190,209,230,253,279,307,337,371,408,449,
    494,544,598,658,724,796,876,963,1060,1166,1282,1411,1552
};
static const uint8_t s_volume_table[17] = {0,1,1,1,2,2,2,3,4,4,5,6,8,9,11,13,16};
static const uint32_t s_clocks[8] = {93750u,125000u,187500u,125000u,46875u,62500u,93750u,62500u};

static inline void lp_fence(void) { __asm__ __volatile__("fence rw,rw" ::: "memory"); }
static inline uint32_t rdcycle32(void) { uint32_t v; __asm__ __volatile__("csrr %0, mcycle" : "=r"(v)); return v; }
static inline int clip12(int v) { if (v > ADPCMMAX) return ADPCMMAX; if (v < ADPCMMIN) return ADPCMMIN; return v; }
static inline int clip16(int v) { if (v > 32767) return 32767; if (v < -32768) return -32768; return v; }

static inline int interp(int A, int B, int C, int y1, int x)
{
    int t = (A * x + FM_IPSCALE/2) / FM_IPSCALE + B;
    t = (t * x + FM_IPSCALE/2) / FM_IPSCALE + C;
    return (t * x + 3*FM_IPSCALE) / (6*FM_IPSCALE) + y1;
}

static void raw_push(int16_t r, int16_t l)
{
    const uint32_t next = s_raw_wr + 1u;
    if ((uint32_t)(next - s_raw_rd) > RAW_LEN) {
        ++lp_adpcm_raw_overruns;
        return;
    }
    s_raw[s_raw_wr & RAW_MASK] = (uint16_t)r | ((uint32_t)(uint16_t)l << 16);
    s_raw_wr = next;
}

static int raw_pop(int16_t *r, int16_t *l)
{
    if (s_raw_rd == s_raw_wr) return 0;
    const uint32_t p = s_raw[s_raw_rd & RAW_MASK];
    ++s_raw_rd;
    *r = (int16_t)(p & 0xffffu);
    *l = (int16_t)(p >> 16);
    return 1;
}

static void decode_nibble(uint8_t val)
{
    const int base = (int)s_base_step[s_step];
    int mag = base / 8;
    if (val & 1u) mag += base / 4;
    if (val & 2u) mag += base / 2;
    if (val & 4u) mag += base;
    s_out += (val & 8u) ? -mag : mag;
    s_out = clip12(s_out);
    s_step += s_index_shift[val & 7u];
    if (s_step < 0) s_step = 0;
    else if (s_step > 48) s_step = 48;

    if (s_outsip[0] == -1) {
        s_outsip[0] = s_outsip[1] = s_outsip[2] = s_outsip[3] = s_out;
    } else {
        s_outsip[0] = s_outsip[1];
        s_outsip[1] = s_outsip[2];
        s_outsip[2] = s_outsip[3];
        s_outsip[3] = s_out;
    }

    int16_t ratios[16];
    int nr = 0;
    while (SAMPLE_RATE_X12 > s_count) {
        if (s_playing && nr < 16) {
            ratios[nr++] = (int16_t)(((s_count / 100u) * FM_IPSCALE) / (SAMPLE_RATE_X12 / 100u));
        }
        s_count += s_clock_rate;
    }
    s_count -= SAMPLE_RATE_X12;
    if (!s_playing || nr == 0) return;

    const int y0=s_outsip[0], y1=s_outsip[1], y2=s_outsip[2], y3=s_outsip[3];
    const int A=-y0+3*y1-3*y2+y3;
    const int B=3*(y0-2*y1+y2);
    const int C=-2*y0-3*y1+6*y2-y3;
    for (int i=0; i<nr; ++i) {
        const int tmp = clip12(interp(A,B,C,y1,ratios[i]));
        const int16_t br = (s_pan & 1u) ? 0 : (int16_t)tmp;
        const int16_t bl = (s_pan & 2u) ? 0 : (int16_t)tmp;
        /* Legacy ADPCM_Update reads BufL into right/output[0], BufR into left/output[1]. */
        raw_push(bl, br);
    }
}

static void state_reset(void)
{
    s_raw_rd = s_raw_wr = 0;
    s_out = 0;
    s_step = 0;
    s_count = 0;
    s_clock = 2u;
    s_clock_rate = s_clocks[s_clock];
    s_pan = 0x0bu;
    s_playing = 0;
    s_volume = 16u;
    s_old_r = s_old_l = 0;
    for (int i=0;i<8;++i) s_outs[i]=0;
    for (int i=0;i<4;++i) { s_outsip[i]=-1; s_outsip_r[i]=0; s_outsip_l[i]=0; }
    lp_adpcm_out_tail = lp_adpcm_out_head = 0;
    lp_adpcm_out_stalls = 0;
    lp_adpcm_raw_overruns = 0;
}

static void set_pan(uint8_t n)
{
    if ((s_pan & 0x0cu) != (n & 0x0cu)) {
        s_count = 0;
        s_clock = (uint8_t)((s_clock & 4u) | ((n >> 2) & 3u));
        s_clock_rate = s_clocks[s_clock];
    }
    s_pan = n;
}

static void set_clock(uint8_t n)
{
    n &= 4u;
    if ((s_clock & 4u) != n) {
        s_count = 0;
        s_clock = (uint8_t)(n | ((s_pan >> 2) & 3u));
        s_clock_rate = s_clocks[s_clock];
    }
}

static void control(uint8_t data)
{
    if (data & 1u) {
        s_playing = 0;
    } else if (data & 2u) {
        if (!s_playing) {
            s_step = 0;
            s_out = 0;
            s_old_l = s_old_r = -2;
            s_playing = 1;
        }
        s_outsip[0]=s_outsip[1]=s_outsip[2]=s_outsip[3]=-1;
    }
}

static void out_push(int16_t r, int16_t l)
{
    for (;;) {
        const uint32_t head = lp_adpcm_out_head;
        const uint32_t tail = lp_adpcm_out_tail;
        if ((uint32_t)(head - tail) < OUT_LEN) {
            lp_adpcm_out_ring[head & OUT_MASK] = (uint16_t)r | ((uint32_t)(uint16_t)l << 16);
            lp_fence();
            lp_adpcm_out_head = head + 1u;
            return;
        }
        ++lp_adpcm_out_stalls;
        lp_fence();
    }
}

static void render_frames(uint32_t frames, uint8_t lpf)
{
    const int vol = (int)s_volume_table[s_volume <= 16u ? s_volume : 16u];
    for (uint32_t i=0; i<frames; ++i) {
        int16_t rr, ll;
        if (raw_pop(&rr, &ll)) {
            s_old_r = rr;
            s_old_l = ll;
        } else {
            rr = (int16_t)s_old_r;
            ll = (int16_t)s_old_l;
        }

        int out_r, out_l;
        if (lpf) {
            int x = (int)rr * 40 * vol;
            int y = (x + s_outs[3]*2 + s_outs[2] + s_outs[1]*157 - s_outs[0]*61) >> 8;
            s_outs[2]=s_outs[3]; s_outs[3]=x; s_outs[0]=s_outs[1]; s_outs[1]=y;
            s_outsip_r[0]=s_outsip_r[1]; s_outsip_r[1]=s_outsip_r[2]; s_outsip_r[2]=s_outsip_r[3]; s_outsip_r[3]=y;
            out_r = clip16(s_outsip_r[1]);

            x = (int)ll * 40 * vol;
            y = (x + s_outs[7]*2 + s_outs[6] + s_outs[5]*157 - s_outs[4]*61) >> 8;
            s_outs[6]=s_outs[7]; s_outs[7]=x; s_outs[4]=s_outs[5]; s_outs[5]=y;
            s_outsip_l[0]=s_outsip_l[1]; s_outsip_l[1]=s_outsip_l[2]; s_outsip_l[2]=s_outsip_l[3]; s_outsip_l[3]=y;
            out_l = clip16(s_outsip_l[1]);
        } else {
            const int x = (int)rr * vol;
            s_outsip_r[0]=s_outsip_r[1]; s_outsip_r[1]=s_outsip_r[2]; s_outsip_r[2]=s_outsip_r[3]; s_outsip_r[3]=x;
            out_r = clip16(s_outsip_r[1]);
            const int y = (int)ll * vol;
            s_outsip_l[0]=s_outsip_l[1]; s_outsip_l[1]=s_outsip_l[2]; s_outsip_l[2]=s_outsip_l[3]; s_outsip_l[3]=y;
            out_l = clip16(s_outsip_l[1]);
        }
        out_push((int16_t)out_r, (int16_t)out_l);
    }
}

static void process_event(uint32_t w0, uint32_t w1)
{
    const uint8_t type=(uint8_t)w0;
    const uint8_t data=(uint8_t)(w0>>8);
    const uint8_t flags=(uint8_t)(w0>>24);
    switch(type) {
        case CMD_RESET: state_reset(); break;
        case CMD_CONTROL: control(data); break;
        case CMD_DATA: if (s_playing) { decode_nibble(data & 15u); decode_nibble(data >> 4); } break;
        case CMD_PAN: set_pan(data); break;
        case CMD_CLOCK: set_clock(data); break;
        case CMD_VOLUME: s_volume = data <= 16u ? data : 16u; break;
        case CMD_RENDER: render_frames(w1, flags & 1u); break;
        default: break;
    }
}

int main(void)
{
    state_reset();
    lp_adpcm_cmd_tail = lp_adpcm_cmd_head = 0;
    lp_adpcm_work_frames = lp_adpcm_work_calls = lp_adpcm_work_us = 0;
    lp_adpcm_heartbeat = 1;
    lp_adpcm_hp_probe = lp_adpcm_lp_echo = 0;
    lp_adpcm_diag_stage = 0x13000010u;
    lp_adpcm_diag_w0 = lp_adpcm_diag_w1 = 0;
    lp_adpcm_diag_mcause = lp_adpcm_diag_mepc = lp_adpcm_diag_mtval = 0;
    lp_fence();
    lp_adpcm_ready = READY_MAGIC;
    lp_fence();

    uint32_t seen_probe = 0;
    for (;;) {
        const uint32_t probe = lp_adpcm_hp_probe;
        lp_fence();
        if (probe != seen_probe) {
            seen_probe = probe;
            lp_adpcm_lp_echo = probe;
            lp_fence();
        }
        if (probe == PROBE_MAGIC) continue;

        while (lp_adpcm_cmd_tail != probe) {
            const uint32_t tail = lp_adpcm_cmd_tail;
            const uint32_t idx = (tail & CMD_MASK) * 2u;
            const uint32_t w0 = lp_adpcm_cmd_ring[idx];
            const uint32_t w1 = lp_adpcm_cmd_ring[idx+1u];
            lp_adpcm_diag_stage = 0x13000020u;
            lp_adpcm_diag_w0 = w0;
            lp_adpcm_diag_w1 = w1;
            lp_fence();
            const uint32_t c0 = rdcycle32();
            if ((uint8_t)w0 == CMD_RESET) {
                state_reset();
            } else {
                process_event(w0,w1);
            }
            const uint32_t dc = rdcycle32() - c0;
            lp_adpcm_work_us += dc / 40u;
            ++lp_adpcm_work_calls;
            if ((uint8_t)w0 == CMD_RENDER) lp_adpcm_work_frames += w1;
            lp_adpcm_cmd_tail = tail + 1u;
            ++lp_adpcm_heartbeat;
            lp_adpcm_diag_stage = 0x13000040u;
            lp_fence();
        }
    }
}
