/*
 * X68K Tab Build 6.15h10-exp: prebuilt bare-metal ESP32-P4 LP-core YM2151 backend.
 *
 * Goal: move the expensive YM2151 waveform loop off HP CPU0 and determine
 * whether the 40 MHz RV32IMAC LP core can materially reduce audio starvation.
 *
 * This is intentionally an experiment, not a production replacement:
 * - synthesis runs at 22050 Hz on the LP core and HP duplicates each stereo
 *   sample to the 44100-Hz mixer (pitch preserved, bandwidth reduced);
 * - register-write boundaries are quantized to 1/22050 s (<= one 44.1-kHz
 *   sample of timing error);
 * - CPU0 backend remains an automatic fallback if LP-core startup fails.
 *
 * The fixed-point hot path is derived from the existing vgmM5 YM2151-only
 * backend in fmgen/vgmm5_ym2151.cpp. Tables that previously required libm are
 * precomputed for 4 MHz / 22050 Hz to keep the LP binary small and deterministic.
 */

#include <stdint.h>
#include <stddef.h>

#define YM_CH 8
#define YM_OPS 32
#define EG_FRACTION_BITS 16
#define EG_MAX ((int32_t)(4096u << EG_FRACTION_BITS))
#define LP_FM_CMD_LEN 128u
#define LP_FM_CMD_MASK (LP_FM_CMD_LEN - 1u)
#define LP_FM_OUT_LEN 1024u
#define LP_FM_OUT_MASK (LP_FM_OUT_LEN - 1u)
#define LP_SHARED __attribute__((section(".lp_shared"), aligned(4)))
#define LP_FM_READY_MAGIC 0x4C50464Du /* "LPFM" handshake */
#define LP_FM_PROBE_MAGIC 0x4850324Cu /* HP->LP proven-path startup probe */

static inline uint32_t lp_cycles(void) { uint32_t v; __asm__ __volatile__("csrr %0, mcycle" : "=r"(v)); return v; }

enum { EG_OFF=0, EG_ATTACK, EG_DECAY, EG_SUSTAIN, EG_RELEASE };
enum {
    LP_FM_WRITE = 1,
    LP_FM_RENDER,
    LP_FM_RESET,
    LP_FM_VOLUME,
    LP_FM_CSM,
    LP_FM_STOP
};

typedef struct {
    uint32_t phase;
    uint32_t phase_step;
    int32_t tl_atten;
    int32_t env_level;
    int32_t ar_step, dr_step, d2r_step, sl_level, rr_step;
    uint8_t env_state;
    uint8_t mul, dt1, dt2;
    uint8_t am_enable;
    uint8_t ar, dr, d2r, rr, ks;
} YMOp;

typedef struct {
    uint32_t output_tick_counter;
    uint8_t pan_l[YM_CH], pan_r[YM_CH];
    uint8_t algo[YM_CH], fb_shift[YM_CH];
    int32_t fb_memory[YM_CH][2];
    int32_t mem_value[YM_CH];
    uint8_t kc[YM_CH], kf[YM_CH], pms[YM_CH], ams[YM_CH];
    YMOp ops[YM_OPS];
    uint32_t lfo_phase, lfo_step;
    int32_t cached_pm, cached_am;
    uint8_t pmd, amd, lfo_wave;
    uint8_t noise_enable;
    uint32_t noise_phase, noise_step, noise_rng;
    int32_t prev_l, prev_r;
    int32_t volume_q14;
    uint8_t active_ch_mask;
} LpYM2151;

/* HP<->LP shared mailbox, deliberately all uint32_t so generated HP headers
 * expose a simple, ABI-safe view. Indices are monotonic SPSC counters. */
LP_SHARED volatile uint32_t lp_fm_ready;
LP_SHARED volatile uint32_t lp_fm_cmd_head;
LP_SHARED volatile uint32_t lp_fm_cmd_tail;
LP_SHARED volatile uint32_t lp_fm_out_head;
LP_SHARED volatile uint32_t lp_fm_out_tail;
LP_SHARED volatile uint32_t lp_fm_cmd_drops;
LP_SHARED volatile uint32_t lp_fm_out_stalls;
LP_SHARED volatile uint32_t lp_fm_work_us;
LP_SHARED volatile uint32_t lp_fm_work_calls;
LP_SHARED volatile uint32_t lp_fm_work_frames44;
LP_SHARED volatile uint32_t lp_fm_profile_us;
LP_SHARED volatile uint32_t lp_fm_profile_calls;
LP_SHARED volatile uint32_t lp_fm_profile_frames44;
LP_SHARED volatile uint32_t lp_fm_heartbeat;
LP_SHARED volatile uint32_t lp_fm_half_phase;
/* Build 6.15h10-exp: plain volatile HP<->LP visibility probe.  This deliberately
 * avoids AMO/libatomic semantics on LP RTC RAM; HP writes hp_probe and LP echoes
 * it from the hot polling loop before command traffic is trusted. */
LP_SHARED volatile uint32_t lp_fm_hp_probe;
LP_SHARED volatile uint32_t lp_fm_lp_echo;
LP_SHARED uint32_t lp_fm_cmd_ring[LP_FM_CMD_LEN * 2u];
LP_SHARED uint32_t lp_fm_out_ring[LP_FM_OUT_LEN];

static LpYM2151 s_ym;
static const int32_t s_pms_mult[8] = {0,1,2,4,8,16,32,64};
static const int32_t s_ams_mult[4] = {0,2,4,8};
static const uint8_t s_kc_to_n[16] = {1,2,3,3,4,5,6,6,7,8,9,9,10,11,12,12};

static const uint16_t s_sin[1024] = {
    2649u, 2243u, 2054u, 1930u, 1837u, 1763u, 1701u, 1649u, 1602u, 1561u, 1524u, 1491u,
    1460u, 1431u, 1405u, 1380u, 1357u, 1336u, 1315u, 1296u, 1277u, 1260u, 1243u, 1227u,
    1211u, 1197u, 1182u, 1169u, 1156u, 1143u, 1131u, 1119u, 1107u, 1096u, 1085u, 1075u,
    1064u, 1054u, 1045u, 1035u, 1026u, 1017u, 1008u, 1000u, 991u, 983u, 975u, 967u,
    959u, 952u, 945u, 937u, 930u, 923u, 916u, 910u, 903u, 897u, 890u, 884u,
    878u, 872u, 866u, 860u, 854u, 849u, 843u, 838u, 832u, 827u, 822u, 817u,
    811u, 806u, 801u, 797u, 792u, 787u, 782u, 778u, 773u, 768u, 764u, 759u,
    755u, 751u, 747u, 742u, 738u, 734u, 730u, 726u, 722u, 718u, 714u, 710u,
    706u, 703u, 699u, 695u, 692u, 688u, 684u, 681u, 677u, 674u, 670u, 667u,
    663u, 660u, 657u, 653u, 650u, 647u, 644u, 641u, 637u, 634u, 631u, 628u,
    625u, 622u, 619u, 616u, 613u, 610u, 607u, 605u, 602u, 599u, 596u, 593u,
    591u, 588u, 585u, 582u, 580u, 577u, 574u, 572u, 569u, 567u, 564u, 561u,
    559u, 556u, 554u, 552u, 549u, 547u, 544u, 542u, 539u, 537u, 535u, 532u,
    530u, 528u, 525u, 523u, 521u, 519u, 516u, 514u, 512u, 510u, 508u, 505u,
    503u, 501u, 499u, 497u, 495u, 493u, 491u, 489u, 487u, 485u, 483u, 481u,
    479u, 477u, 475u, 473u, 471u, 469u, 467u, 465u, 463u, 461u, 459u, 457u,
    455u, 454u, 452u, 450u, 448u, 446u, 444u, 443u, 441u, 439u, 437u, 436u,
    434u, 432u, 430u, 429u, 427u, 425u, 423u, 422u, 420u, 418u, 417u, 415u,
    413u, 412u, 410u, 409u, 407u, 405u, 404u, 402u, 401u, 399u, 397u, 396u,
    394u, 393u, 391u, 390u, 388u, 387u, 385u, 384u, 382u, 381u, 379u, 378u,
    376u, 375u, 373u, 372u, 370u, 369u, 368u, 366u, 365u, 363u, 362u, 360u,
    359u, 358u, 356u, 355u, 354u, 352u, 351u, 350u, 348u, 347u, 345u, 344u,
    343u, 342u, 340u, 339u, 338u, 336u, 335u, 334u, 332u, 331u, 330u, 329u,
    327u, 326u, 325u, 324u, 322u, 321u, 320u, 319u, 318u, 316u, 315u, 314u,
    313u, 312u, 310u, 309u, 308u, 307u, 306u, 304u, 303u, 302u, 301u, 300u,
    299u, 298u, 296u, 295u, 294u, 293u, 292u, 291u, 290u, 289u, 288u, 286u,
    285u, 284u, 283u, 282u, 281u, 280u, 279u, 278u, 277u, 276u, 275u, 274u,
    273u, 271u, 270u, 269u, 268u, 267u, 266u, 265u, 264u, 263u, 262u, 261u,
    260u, 259u, 258u, 257u, 256u, 255u, 254u, 253u, 252u, 251u, 250u, 250u,
    249u, 248u, 247u, 246u, 245u, 244u, 243u, 242u, 241u, 240u, 239u, 238u,
    237u, 236u, 235u, 235u, 234u, 233u, 232u, 231u, 230u, 229u, 228u, 227u,
    227u, 226u, 225u, 224u, 223u, 222u, 221u, 220u, 220u, 219u, 218u, 217u,
    216u, 215u, 214u, 214u, 213u, 212u, 211u, 210u, 209u, 209u, 208u, 207u,
    206u, 205u, 205u, 204u, 203u, 202u, 201u, 201u, 200u, 199u, 198u, 197u,
    197u, 196u, 195u, 194u, 194u, 193u, 192u, 191u, 190u, 190u, 189u, 188u,
    187u, 187u, 186u, 185u, 184u, 184u, 183u, 182u, 181u, 181u, 180u, 179u,
    179u, 178u, 177u, 176u, 176u, 175u, 174u, 174u, 173u, 172u, 171u, 171u,
    170u, 169u, 169u, 168u, 167u, 167u, 166u, 165u, 164u, 164u, 163u, 162u,
    162u, 161u, 160u, 160u, 159u, 158u, 158u, 157u, 156u, 156u, 155u, 155u,
    154u, 153u, 153u, 152u, 151u, 151u, 150u, 149u, 149u, 148u, 147u, 147u,
    146u, 146u, 145u, 144u, 144u, 143u, 143u, 142u, 141u, 141u, 140u, 139u,
    139u, 138u, 138u, 137u, 136u, 136u, 135u, 135u, 134u, 134u, 133u, 132u,
    132u, 131u, 131u, 130u, 129u, 129u, 128u, 128u, 127u, 127u, 126u, 126u,
    125u, 124u, 124u, 123u, 123u, 122u, 122u, 121u, 121u, 120u, 119u, 119u,
    118u, 118u, 117u, 117u, 116u, 116u, 115u, 115u, 114u, 114u, 113u, 113u,
    112u, 112u, 111u, 110u, 110u, 109u, 109u, 108u, 108u, 107u, 107u, 106u,
    106u, 105u, 105u, 104u, 104u, 103u, 103u, 102u, 102u, 101u, 101u, 101u,
    100u, 100u, 99u, 99u, 98u, 98u, 97u, 97u, 96u, 96u, 95u, 95u,
    94u, 94u, 93u, 93u, 93u, 92u, 92u, 91u, 91u, 90u, 90u, 89u,
    89u, 88u, 88u, 88u, 87u, 87u, 86u, 86u, 85u, 85u, 84u, 84u,
    84u, 83u, 83u, 82u, 82u, 81u, 81u, 81u, 80u, 80u, 79u, 79u,
    79u, 78u, 78u, 77u, 77u, 77u, 76u, 76u, 75u, 75u, 74u, 74u,
    74u, 73u, 73u, 72u, 72u, 72u, 71u, 71u, 71u, 70u, 70u, 69u,
    69u, 69u, 68u, 68u, 67u, 67u, 67u, 66u, 66u, 66u, 65u, 65u,
    64u, 64u, 64u, 63u, 63u, 63u, 62u, 62u, 62u, 61u, 61u, 61u,
    60u, 60u, 59u, 59u, 59u, 58u, 58u, 58u, 57u, 57u, 57u, 56u,
    56u, 56u, 55u, 55u, 55u, 54u, 54u, 54u, 53u, 53u, 53u, 52u,
    52u, 52u, 51u, 51u, 51u, 50u, 50u, 50u, 49u, 49u, 49u, 49u,
    48u, 48u, 48u, 47u, 47u, 47u, 46u, 46u, 46u, 45u, 45u, 45u,
    45u, 44u, 44u, 44u, 43u, 43u, 43u, 42u, 42u, 42u, 42u, 41u,
    41u, 41u, 40u, 40u, 40u, 40u, 39u, 39u, 39u, 39u, 38u, 38u,
    38u, 37u, 37u, 37u, 37u, 36u, 36u, 36u, 36u, 35u, 35u, 35u,
    35u, 34u, 34u, 34u, 34u, 33u, 33u, 33u, 33u, 32u, 32u, 32u,
    32u, 31u, 31u, 31u, 31u, 30u, 30u, 30u, 30u, 29u, 29u, 29u,
    29u, 28u, 28u, 28u, 28u, 27u, 27u, 27u, 27u, 27u, 26u, 26u,
    26u, 26u, 25u, 25u, 25u, 25u, 25u, 24u, 24u, 24u, 24u, 24u,
    23u, 23u, 23u, 23u, 22u, 22u, 22u, 22u, 22u, 21u, 21u, 21u,
    21u, 21u, 20u, 20u, 20u, 20u, 20u, 19u, 19u, 19u, 19u, 19u,
    19u, 18u, 18u, 18u, 18u, 18u, 17u, 17u, 17u, 17u, 17u, 17u,
    16u, 16u, 16u, 16u, 16u, 15u, 15u, 15u, 15u, 15u, 15u, 14u,
    14u, 14u, 14u, 14u, 14u, 14u, 13u, 13u, 13u, 13u, 13u, 13u,
    12u, 12u, 12u, 12u, 12u, 12u, 12u, 11u, 11u, 11u, 11u, 11u,
    11u, 11u, 10u, 10u, 10u, 10u, 10u, 10u, 10u, 9u, 9u, 9u,
    9u, 9u, 9u, 9u, 9u, 8u, 8u, 8u, 8u, 8u, 8u, 8u,
    8u, 7u, 7u, 7u, 7u, 7u, 7u, 7u, 7u, 6u, 6u, 6u,
    6u, 6u, 6u, 6u, 6u, 6u, 6u, 5u, 5u, 5u, 5u, 5u,
    5u, 5u, 5u, 5u, 5u, 4u, 4u, 4u, 4u, 4u, 4u, 4u,
    4u, 4u, 4u, 4u, 3u, 3u, 3u, 3u, 3u, 3u, 3u, 3u,
    3u, 3u, 3u, 3u, 3u, 2u, 2u, 2u, 2u, 2u, 2u, 2u,
    2u, 2u, 2u, 2u, 2u, 2u, 2u, 2u, 1u, 1u, 1u, 1u,
    1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u, 1u,
    1u, 1u, 1u, 1u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u, 0u,
    0u, 0u, 0u, 0u,
};

static const uint16_t s_exp[256] = {
    16384u, 16339u, 16295u, 16251u, 16207u, 16163u, 16119u, 16076u, 16032u, 15989u, 15946u, 15903u,
    15860u, 15817u, 15774u, 15731u, 15689u, 15646u, 15604u, 15562u, 15520u, 15478u, 15436u, 15394u,
    15353u, 15311u, 15270u, 15228u, 15187u, 15146u, 15105u, 15064u, 15024u, 14983u, 14943u, 14902u,
    14862u, 14822u, 14782u, 14742u, 14702u, 14662u, 14622u, 14583u, 14543u, 14504u, 14465u, 14426u,
    14387u, 14348u, 14309u, 14270u, 14232u, 14193u, 14155u, 14117u, 14078u, 14040u, 14002u, 13965u,
    13927u, 13889u, 13852u, 13814u, 13777u, 13739u, 13702u, 13665u, 13628u, 13591u, 13555u, 13518u,
    13482u, 13445u, 13409u, 13372u, 13336u, 13300u, 13264u, 13228u, 13193u, 13157u, 13121u, 13086u,
    13051u, 13015u, 12980u, 12945u, 12910u, 12875u, 12840u, 12805u, 12771u, 12736u, 12702u, 12668u,
    12633u, 12599u, 12565u, 12531u, 12497u, 12463u, 12430u, 12396u, 12363u, 12329u, 12296u, 12263u,
    12229u, 12196u, 12163u, 12130u, 12098u, 12065u, 12032u, 12000u, 11967u, 11935u, 11903u, 11871u,
    11838u, 11806u, 11774u, 11743u, 11711u, 11679u, 11648u, 11616u, 11585u, 11553u, 11522u, 11491u,
    11460u, 11429u, 11398u, 11367u, 11336u, 11306u, 11275u, 11245u, 11214u, 11184u, 11154u, 11124u,
    11094u, 11064u, 11034u, 11004u, 10974u, 10944u, 10915u, 10885u, 10856u, 10826u, 10797u, 10768u,
    10739u, 10710u, 10681u, 10652u, 10623u, 10594u, 10566u, 10537u, 10509u, 10480u, 10452u, 10424u,
    10396u, 10367u, 10339u, 10311u, 10284u, 10256u, 10228u, 10200u, 10173u, 10145u, 10118u, 10090u,
    10063u, 10036u, 10009u, 9982u, 9955u, 9928u, 9901u, 9874u, 9848u, 9821u, 9794u, 9768u,
    9741u, 9715u, 9689u, 9663u, 9637u, 9610u, 9584u, 9559u, 9533u, 9507u, 9481u, 9456u,
    9430u, 9405u, 9379u, 9354u, 9328u, 9303u, 9278u, 9253u, 9228u, 9203u, 9178u, 9153u,
    9129u, 9104u, 9079u, 9055u, 9030u, 9006u, 8981u, 8957u, 8933u, 8909u, 8885u, 8861u,
    8837u, 8813u, 8789u, 8765u, 8742u, 8718u, 8694u, 8671u, 8647u, 8624u, 8601u, 8577u,
    8554u, 8531u, 8508u, 8485u, 8462u, 8439u, 8416u, 8394u, 8371u, 8348u, 8326u, 8303u,
    8281u, 8258u, 8236u, 8214u,
};

static const uint32_t s_rate[128] = {
    0u, 76u, 90u, 107u, 128u, 152u, 181u, 215u,
    256u, 304u, 362u, 430u, 512u, 608u, 724u, 861u,
    1024u, 1217u, 1448u, 1722u, 2048u, 2435u, 2896u, 3444u,
    4096u, 4870u, 5792u, 6888u, 8192u, 9741u, 11585u, 13777u,
    16384u, 19483u, 23170u, 27554u, 32768u, 38967u, 46340u, 55108u,
    65536u, 77935u, 92681u, 110217u, 131072u, 155871u, 185363u, 220435u,
    262144u, 311743u, 370727u, 440871u, 524288u, 623487u, 741455u, 881743u,
    1048576u, 1246974u, 1482910u, 1763487u, 2097152u, 2493948u, 2965820u, 3526975u,
    4194304u, 4987896u, 5931641u, 7053950u, 8388608u, 9975792u, 11863283u, 14107900u,
    16777216u, 19951584u, 23726566u, 28215801u, 33554432u, 33554432u, 33554432u, 33554432u,
    33554432u, 33554432u, 33554432u, 33554432u, 33554432u, 33554432u, 33554432u, 33554432u,
    33554432u, 33554432u, 33554432u, 33554432u, 33554432u, 33554432u, 33554432u, 33554432u,
    33554432u, 33554432u, 33554432u, 33554432u, 33554432u, 33554432u, 33554432u, 33554432u,
    33554432u, 33554432u, 33554432u, 33554432u, 33554432u, 33554432u, 33554432u, 33554432u,
    33554432u, 33554432u, 33554432u, 33554432u, 33554432u, 33554432u, 33554432u, 33554432u,
    33554432u, 33554432u, 33554432u, 33554432u, 33554432u, 33554432u, 33554432u, 33554432u,
};

static const uint32_t s_freq[832] = {
    3559128u, 3562342u, 3565558u, 3568778u, 3572000u, 3575226u,
    3578454u, 3581685u, 3584919u, 3588156u, 3591396u, 3594639u,
    3597885u, 3601133u, 3604385u, 3607639u, 3610897u, 3614157u,
    3617421u, 3620687u, 3623956u, 3627229u, 3630504u, 3633782u,
    3637063u, 3640347u, 3643634u, 3646924u, 3650217u, 3653513u,
    3656812u, 3660114u, 3663419u, 3666726u, 3670037u, 3673351u,
    3676668u, 3679988u, 3683311u, 3686636u, 3689965u, 3693297u,
    3696632u, 3699970u, 3703311u, 3706654u, 3710001u, 3713351u,
    3716704u, 3720060u, 3723419u, 3726781u, 3730146u, 3733514u,
    3736886u, 3740260u, 3743637u, 3747017u, 3750401u, 3753787u,
    3757177u, 3760569u, 3763965u, 3767363u, 3770765u, 3774170u,
    3777578u, 3780989u, 3784403u, 3787820u, 3791240u, 3794663u,
    3798089u, 3801519u, 3804951u, 3808387u, 3811826u, 3815268u,
    3818713u, 3822161u, 3825612u, 3829066u, 3832524u, 3835984u,
    3839448u, 3842915u, 3846385u, 3849858u, 3853334u, 3856813u,
    3860296u, 3863781u, 3867270u, 3870762u, 3874257u, 3877755u,
    3881257u, 3884761u, 3888269u, 3891780u, 3895294u, 3898811u,
    3902332u, 3905855u, 3909382u, 3912912u, 3916445u, 3919981u,
    3923521u, 3927064u, 3930610u, 3934159u, 3937711u, 3941267u,
    3944825u, 3948387u, 3951952u, 3955521u, 3959092u, 3962667u,
    3966245u, 3969827u, 3973411u, 3976999u, 3980590u, 3984184u,
    3987782u, 3991382u, 3994986u, 3998594u, 4002204u, 4005818u,
    4009435u, 4013055u, 4016679u, 4020306u, 4023936u, 4027569u,
    4031206u, 4034846u, 4038489u, 4042135u, 4045785u, 4049438u,
    4053095u, 4056754u, 4060417u, 4064084u, 4067753u, 4071426u,
    4075103u, 4078782u, 4082465u, 4086151u, 4089841u, 4093534u,
    4097230u, 4100930u, 4104633u, 4108339u, 4112048u, 4115761u,
    4119478u, 4123197u, 4126920u, 4130647u, 4134376u, 4138110u,
    4141846u, 4145586u, 4149329u, 4153076u, 4156826u, 4160579u,
    4164336u, 4168096u, 4171860u, 4175626u, 4179397u, 4183171u,
    4186948u, 4190728u, 4194512u, 4198300u, 4202091u, 4205885u,
    4209682u, 4213484u, 4217288u, 4221096u, 4224907u, 4228722u,
    4232541u, 4236362u, 4240188u, 4244016u, 4247848u, 4251684u,
    4255523u, 4259365u, 4263211u, 4267061u, 4270914u, 4274770u,
    4278630u, 4282493u, 4286360u, 4290231u, 4294104u, 4297982u,
    4301863u, 4305747u, 4309635u, 4313526u, 4317421u, 4321319u,
    4325221u, 4329127u, 4333036u, 4336948u, 4340864u, 4344784u,
    4348707u, 4352633u, 4356564u, 4360497u, 4364435u, 4368375u,
    4372320u, 4376268u, 4380219u, 4384174u, 4388133u, 4392095u,
    4396061u, 4400030u, 4404003u, 4407980u, 4411960u, 4415944u,
    4419931u, 4423922u, 4427917u, 4431915u, 4435917u, 4439922u,
    4443931u, 4447944u, 4451960u, 4455980u, 4460003u, 4464030u,
    4468061u, 4472096u, 4476134u, 4480175u, 4484221u, 4488270u,
    4492322u, 4496379u, 4500439u, 4504502u, 4508569u, 4512640u,
    4516715u, 4520793u, 4524875u, 4528961u, 4533051u, 4537144u,
    4541240u, 4545341u, 4549445u, 4553553u, 4557665u, 4561780u,
    4565899u, 4570022u, 4574148u, 4578278u, 4582412u, 4586550u,
    4590691u, 4594836u, 4598985u, 4603138u, 4607294u, 4611454u,
    4615618u, 4619786u, 4623957u, 4628133u, 4632311u, 4636494u,
    4640681u, 4644871u, 4649065u, 4653263u, 4657464u, 4661670u,
    4665879u, 4670092u, 4674309u, 4678530u, 4682754u, 4686982u,
    4691214u, 4695450u, 4699690u, 4703934u, 4708181u, 4712432u,
    4716687u, 4720946u, 4725209u, 4729475u, 4733746u, 4738020u,
    4742298u, 4746580u, 4750866u, 4755156u, 4759450u, 4763747u,
    4768049u, 4772354u, 4776663u, 4780976u, 4785293u, 4789614u,
    4793939u, 4798267u, 4802600u, 4806936u, 4811277u, 4815621u,
    4819969u, 4824321u, 4828677u, 4833037u, 4837401u, 4841769u,
    4846141u, 4850517u, 4854897u, 4859280u, 4863668u, 4868060u,
    4872455u, 4876855u, 4881258u, 4885666u, 4890077u, 4894493u,
    4898912u, 4903336u, 4907763u, 4912194u, 4916630u, 4921069u,
    4925513u, 4929960u, 4934412u, 4938867u, 4943327u, 4947790u,
    4952258u, 4956729u, 4961205u, 4965685u, 4970169u, 4974656u,
    4979148u, 4983644u, 4988144u, 4992648u, 4997156u, 5001668u,
    5006184u, 5010705u, 5015229u, 5019758u, 5024290u, 5028827u,
    5033368u, 5037912u, 5042461u, 5047014u, 5051572u, 5056133u,
    5060698u, 5065268u, 5069841u, 5074419u, 5079001u, 5083587u,
    5088177u, 5092772u, 5097370u, 5101973u, 5106580u, 5111190u,
    5115806u, 5120425u, 5125048u, 5129676u, 5134308u, 5138944u,
    5143584u, 5148228u, 5152877u, 5157530u, 5162187u, 5166848u,
    5171513u, 5176183u, 5180856u, 5185534u, 5190217u, 5194903u,
    5199594u, 5204289u, 5208988u, 5213691u, 5218399u, 5223111u,
    5227827u, 5232548u, 5237272u, 5242001u, 5246734u, 5251472u,
    5256214u, 5260960u, 5265710u, 5270465u, 5275224u, 5279987u,
    5284754u, 5289526u, 5294302u, 5299083u, 5303868u, 5308657u,
    5313450u, 5318248u, 5323050u, 5327856u, 5332667u, 5337482u,
    5342302u, 5347125u, 5351954u, 5356786u, 5361623u, 5366464u,
    5371310u, 5376160u, 5381014u, 5385873u, 5390736u, 5395604u,
    5400476u, 5405352u, 5410233u, 5415118u, 5420007u, 5424901u,
    5429800u, 5434702u, 5439610u, 5444521u, 5449437u, 5454358u,
    5459283u, 5464212u, 5469146u, 5474084u, 5479027u, 5483975u,
    5488926u, 5493882u, 5498843u, 5503808u, 5508778u, 5513752u,
    5518731u, 5523714u, 5528701u, 5533693u, 5538690u, 5543691u,
    5548697u, 5553707u, 5558722u, 5563741u, 5568765u, 5573793u,
    5578826u, 5583863u, 5588905u, 5593951u, 5599002u, 5604058u,
    5609118u, 5614183u, 5619252u, 5624326u, 5629404u, 5634487u,
    5639575u, 5644667u, 5649764u, 5654865u, 5659971u, 5665082u,
    5670197u, 5675317u, 5680442u, 5685571u, 5690705u, 5695843u,
    5700986u, 5706134u, 5711286u, 5716443u, 5721605u, 5726771u,
    5731942u, 5737117u, 5742298u, 5747483u, 5752672u, 5757867u,
    5763066u, 5768269u, 5773478u, 5778691u, 5783909u, 5789131u,
    5794359u, 5799591u, 5804827u, 5810069u, 5815315u, 5820566u,
    5825821u, 5831082u, 5836347u, 5841617u, 5846891u, 5852171u,
    5857455u, 5862744u, 5868038u, 5873336u, 5878639u, 5883947u,
    5889260u, 5894578u, 5899900u, 5905228u, 5910560u, 5915897u,
    5921238u, 5926585u, 5931936u, 5937293u, 5942654u, 5948019u,
    5953390u, 5958766u, 5964146u, 5969531u, 5974922u, 5980317u,
    5985717u, 5991121u, 5996531u, 6001945u, 6007365u, 6012789u,
    6018218u, 6023652u, 6029092u, 6034535u, 6039984u, 6045438u,
    6050897u, 6056360u, 6061829u, 6067302u, 6072781u, 6078264u,
    6083752u, 6089246u, 6094744u, 6100247u, 6105755u, 6111269u,
    6116787u, 6122310u, 6127838u, 6133371u, 6138909u, 6144452u,
    6150000u, 6155553u, 6161111u, 6166675u, 6172243u, 6177816u,
    6183394u, 6188977u, 6194566u, 6200159u, 6205757u, 6211361u,
    6216969u, 6222583u, 6228202u, 6233825u, 6239454u, 6245088u,
    6250727u, 6256371u, 6262020u, 6267674u, 6273334u, 6278998u,
    6284668u, 6290342u, 6296022u, 6301707u, 6307397u, 6313092u,
    6318793u, 6324498u, 6330209u, 6335925u, 6341646u, 6347372u,
    6353103u, 6358840u, 6364581u, 6370328u, 6376080u, 6381838u,
    6387600u, 6393368u, 6399140u, 6404919u, 6410702u, 6416490u,
    6422284u, 6428083u, 6433887u, 6439697u, 6445511u, 6451331u,
    6457156u, 6462987u, 6468823u, 6474664u, 6480510u, 6486361u,
    6492218u, 6498080u, 6503948u, 6509820u, 6515698u, 6521582u,
    6527470u, 6533364u, 6539263u, 6545168u, 6551078u, 6556993u,
    6562914u, 6568840u, 6574771u, 6580708u, 6586650u, 6592597u,
    6598550u, 6604508u, 6610471u, 6616440u, 6622414u, 6628394u,
    6634379u, 6640370u, 6646366u, 6652367u, 6658374u, 6664386u,
    6670403u, 6676426u, 6682455u, 6688489u, 6694528u, 6700573u,
    6706623u, 6712679u, 6718740u, 6724806u, 6730878u, 6736956u,
    6743039u, 6749128u, 6755222u, 6761321u, 6767426u, 6773537u,
    6779653u, 6785775u, 6791902u, 6798035u, 6804173u, 6810317u,
    6816466u, 6822621u, 6828781u, 6834947u, 6841119u, 6847296u,
    6853479u, 6859667u, 6865861u, 6872060u, 6878266u, 6884476u,
    6890692u, 6896914u, 6903142u, 6909375u, 6915614u, 6921858u,
    6928108u, 6934364u, 6940625u, 6946892u, 6953165u, 6959443u,
    6965727u, 6972017u, 6978312u, 6984613u, 6990920u, 6997232u,
    7003550u, 7009874u, 7016204u, 7022539u, 7028880u, 7035227u,
    7041579u, 7047937u, 7054301u, 7060671u, 7067046u, 7073427u,
    7079814u, 7086207u, 7092605u, 7099009u, 7105419u, 7111835u,
    7118257u, 7124684u, 7131117u, 7137556u, 7144001u, 7150452u,
    7156908u, 7163370u, 7169839u, 7176313u, 7182792u, 7189278u,
    7195770u, 7202267u, 7208770u, 7215279u, 7221794u, 7228315u,
    7234842u, 7241374u, 7247913u, 7254457u, 7261008u, 7267564u,
    7274126u, 7280694u, 7287268u, 7293848u, 7300434u, 7307026u,
    7313624u, 7320228u, 7326838u, 7333453u, 7340075u, 7346703u,
    7353336u, 7359976u, 7366622u, 7373273u, 7379931u, 7386595u,
    7393264u, 7399940u, 7406622u, 7413309u, 7420003u, 7426703u,
    7433409u, 7440121u, 7446839u, 7453563u, 7460293u, 7467029u,
    7473772u, 7480520u, 7487274u, 7494035u, 7500802u, 7507575u,
    7514353u, 7521139u, 7527930u, 7534727u,
};

static const uint32_t s_dt1_q16[8] = {
    65536u, 65667u, 65798u, 65929u, 65536u, 65405u, 65274u, 65143u,
};

static const uint32_t s_dt2_step[4] = {
    0u, 37398354u, 48695774u, 59214061u,
};

static const uint32_t s_noise_step[32] = {
    760871473u, 785415715u, 811596238u, 839582316u, 869567398u, 901773598u, 936457198u, 973915486u,
    1014495298u, 1058603789u, 1106722143u, 1159423198u, 1217394358u, 1281467745u, 1352660398u, 1432228656u,
    1521742947u, 1623192477u, 1739134797u, 1872914397u, 2028990597u, 2213444287u, 2434788716u, 2705320796u,
    3043485895u, 3478269595u, 4057981194u, 4294967295u, 4294967295u, 4294967295u, 4294967295u, 4294967295u,
};

static const uint32_t s_lfo_step[256] = {
    29217u, 58434u, 87652u, 116869u, 146087u, 175304u, 204522u, 233739u,
    262957u, 292174u, 321392u, 350609u, 379827u, 409044u, 438261u, 467479u,
    496696u, 525914u, 555131u, 584349u, 613566u, 642784u, 672001u, 701219u,
    730436u, 759654u, 788871u, 818089u, 847306u, 876523u, 905741u, 934958u,
    964176u, 993393u, 1022611u, 1051828u, 1081046u, 1110263u, 1139481u, 1168698u,
    1197916u, 1227133u, 1256350u, 1285568u, 1314785u, 1344003u, 1373220u, 1402438u,
    1431655u, 1460873u, 1490090u, 1519308u, 1548525u, 1577743u, 1606960u, 1636178u,
    1665395u, 1694612u, 1723830u, 1753047u, 1782265u, 1811482u, 1840700u, 1869917u,
    1899135u, 1928352u, 1957570u, 1986787u, 2016005u, 2045222u, 2074439u, 2103657u,
    2132874u, 2162092u, 2191309u, 2220527u, 2249744u, 2278962u, 2308179u, 2337397u,
    2366614u, 2395832u, 2425049u, 2454267u, 2483484u, 2512701u, 2541919u, 2571136u,
    2600354u, 2629571u, 2658789u, 2688006u, 2717224u, 2746441u, 2775659u, 2804876u,
    2834094u, 2863311u, 2892528u, 2921746u, 2950963u, 2980181u, 3009398u, 3038616u,
    3067833u, 3097051u, 3126268u, 3155486u, 3184703u, 3213921u, 3243138u, 3272356u,
    3301573u, 3330790u, 3360008u, 3389225u, 3418443u, 3447660u, 3476878u, 3506095u,
    3535313u, 3564530u, 3593748u, 3622965u, 3652183u, 3681400u, 3710618u, 3739835u,
    3769052u, 3798270u, 3827487u, 3856705u, 3885922u, 3915140u, 3944357u, 3973575u,
    4002792u, 4032010u, 4061227u, 4090445u, 4119662u, 4148879u, 4178097u, 4207314u,
    4236532u, 4265749u, 4294967u, 4324184u, 4353402u, 4382619u, 4411837u, 4441054u,
    4470272u, 4499489u, 4528707u, 4557924u, 4587141u, 4616359u, 4645576u, 4674794u,
    4704011u, 4733229u, 4762446u, 4791664u, 4820881u, 4850099u, 4879316u, 4908534u,
    4937751u, 4966968u, 4996186u, 5025403u, 5054621u, 5083838u, 5113056u, 5142273u,
    5171491u, 5200708u, 5229926u, 5259143u, 5288361u, 5317578u, 5346796u, 5376013u,
    5405230u, 5434448u, 5463665u, 5492883u, 5522100u, 5551318u, 5580535u, 5609753u,
    5638970u, 5668188u, 5697405u, 5726623u, 5755840u, 5785057u, 5814275u, 5843492u,
    5872710u, 5901927u, 5931145u, 5960362u, 5989580u, 6018797u, 6048015u, 6077232u,
    6106450u, 6135667u, 6164885u, 6194102u, 6223319u, 6252537u, 6281754u, 6310972u,
    6340189u, 6369407u, 6398624u, 6427842u, 6457059u, 6486277u, 6515494u, 6544712u,
    6573929u, 6603146u, 6632364u, 6661581u, 6690799u, 6720016u, 6749234u, 6778451u,
    6807669u, 6836886u, 6866104u, 6895321u, 6924539u, 6953756u, 6982974u, 7012191u,
    7041408u, 7070626u, 7099843u, 7129061u, 7158278u, 7187496u, 7216713u, 7245931u,
    7275148u, 7304366u, 7333583u, 7362801u, 7392018u, 7421236u, 7450453u, 7479670u,
};

static const int32_t s_volume_q14[17] = {
    0, 518, 652, 821, 1033, 1301, 1638, 2062,
    2596, 3269, 4115, 5181, 6522, 8211, 10337, 13014,
    16384,
};


static inline int32_t lfo_pm_value(uint8_t wave, uint8_t p)
{
    switch (wave & 3u) {
    case 0: return (int32_t)p - 128;
    case 1: return p < 128 ? 127 : -128;
    case 2:
        if (p < 64) return (int32_t)p * 2;
        if (p < 192) return 127 - ((int32_t)p - 64) * 2;
        return -127 + ((int32_t)p - 192) * 2;
    default: {
        uint32_t r = ((uint32_t)p * 1103515245u + 12345u) >> 16;
        return (int8_t)((r & 255u) - 128u);
    }
    }
}

static inline int32_t lfo_am_value(uint8_t wave, uint8_t p)
{
    switch (wave & 3u) {
    case 0: return 255 - p;
    case 1: return p < 128 ? 255 : 0;
    case 2: return p < 128 ? (255 - (int32_t)p * 2) : (((int32_t)p - 128) * 2);
    default: {
        uint32_t r = ((uint32_t)p * 1103515245u + 12345u) >> 16;
        return (int32_t)(r & 255u);
    }
    }
}

static void reset_state(void)
{
    int32_t vol = s_ym.volume_q14;
    { volatile uint32_t *p=(volatile uint32_t *)(void *)&s_ym; uint32_t n=(uint32_t)((sizeof(s_ym)+3u)/4u); while(n--) *p++=0; }
    s_ym.volume_q14 = vol;
    s_ym.noise_rng = 1;
    /* Current 5.42 default LFO is 5 Hz. */
    s_ym.lfo_step = 973915u;
    for (int ch=0; ch<YM_CH; ++ch) { s_ym.pan_l[ch]=1; s_ym.pan_r[ch]=1; }
    for (int i=0; i<YM_OPS; ++i) {
        s_ym.ops[i].env_state = EG_OFF;
        s_ym.ops[i].env_level = EG_MAX;
    }
}

static void update_phase(LpYM2151 *e, int ch)
{
    int note=e->kc[ch]&15, oct=(e->kc[ch]>>4)&7, kf=e->kf[ch]&63;
    uint32_t base=s_freq[(uint32_t)s_kc_to_n[note]*64u+(uint32_t)kf] << oct;
    for(int o=0;o<4;o++) {
        YMOp *op=&e->ops[ch*4+o];
        uint32_t mul2=op->mul ? ((uint32_t)op->mul * 2u) : 1u;
        uint64_t v=(uint64_t)base * (uint64_t)mul2 * (uint64_t)s_dt1_q16[op->dt1&7u];
        op->phase_step=(uint32_t)(v >> 17) + s_dt2_step[op->dt2&3u];
    }
}

static void update_rates(LpYM2151 *e,int ch)
{
    for(int o=0;o<4;o++) {
        YMOp *p=&e->ops[ch*4+o];
        int shift=5-(int)p->ks;
        int ks= shift >= 0 ? (e->kc[ch] >> shift) : e->kc[ch];
        int r=p->ar? p->ar*2+ks:0; if(r>127)r=127; p->ar_step=(int32_t)(s_rate[r]*8u); if(!p->ar_step&&r)p->ar_step=1;
        r=p->dr*2+ks; if(r>127)r=127; p->dr_step=(int32_t)s_rate[r]; if(!p->dr_step&&r)p->dr_step=1;
        r=p->d2r*2+ks; if(r>127)r=127; p->d2r_step=(int32_t)s_rate[r]; if(!p->d2r_step&&r)p->d2r_step=1;
        r=p->rr*2+ks; if(r>127)r=127; p->rr_step=(int32_t)s_rate[r]; if(!p->rr_step&&r)p->rr_step=1;
    }
}

static __attribute__((noinline)) void update_envelopes(LpYM2151 *e)
{
    for(int i=0;i<YM_OPS;i++) {
        YMOp *p=&e->ops[i];
        if(p->env_state==EG_OFF)continue;
        switch(p->env_state) {
        case EG_ATTACK: {
            uint32_t d=(uint32_t)p->ar_step;
            if(d>=s_rate[127]) { p->env_level=0; p->env_state=EG_DECAY; break; }
            uint32_t ev=(uint32_t)p->env_level;
            uint32_t drop=(d>65535u)? d+(ev>>1) : d+((d*(ev>>12))>>5);
            if(!drop&&d)drop=1;
            if(ev<=drop) { p->env_level=0; p->env_state=EG_DECAY; } else p->env_level=(int32_t)(ev-drop);
            break; }
        case EG_DECAY:
            p->env_level+=p->dr_step;
            if(p->env_level>=p->sl_level){p->env_level=p->sl_level;p->env_state=EG_SUSTAIN;}
            break;
        case EG_SUSTAIN:
            p->env_level+=p->d2r_step; if(p->env_level>=EG_MAX)p->env_level=EG_MAX; break;
        case EG_RELEASE:
            p->env_level+=p->rr_step; if(p->env_level>=EG_MAX){p->env_level=EG_MAX;p->env_state=EG_OFF;} break;
        default: break;
        }
    }
    uint8_t mask=0;
    for(int ch=0;ch<YM_CH;ch++) {
        const YMOp *p=&e->ops[ch*4];
        if(p[0].env_state!=EG_OFF || p[1].env_state!=EG_OFF ||
           p[2].env_state!=EG_OFF || p[3].env_state!=EG_OFF)
            mask |= (uint8_t)(1u<<ch);
    }
    e->active_ch_mask=mask;
}

static inline int32_t feedback(const LpYM2151 *e,int ch)
{
    uint32_t f=e->fb_shift[ch]&7u; if(!f)return 0;
    return (e->fb_memory[ch][0]+e->fb_memory[ch][1]) >> (9u-f);
}

static inline int32_t calc_op(LpYM2151 *e,YMOp *p,int32_t mod,int32_t pm,int32_t am,int noise)
{
    if(p->env_state==EG_OFF)return 0;
    uint32_t step=p->phase_step;
    mod*=2;
    if(pm) step+=(uint32_t)((((int32_t)(step>>10))*pm)>>3);
    p->phase+=step;
    int32_t atten=(p->env_level>>EG_FRACTION_BITS)+p->tl_atten;
    if(p->am_enable && am) atten+=am;
    if(atten>=3840)return 0;
    uint32_t ph=p->phase>>20;
    uint32_t wi=(ph+(uint32_t)mod)&0xfffu;
    if(noise) {
        int32_t nv=((int32_t)(e->noise_rng&0x7fffu)-16384)>>1;
        nv=(nv*3)>>2;
        uint32_t ex=s_exp[(uint32_t)atten&255u] >> ((uint32_t)atten>>8);
        return (nv*(int32_t)ex)>>14;
    }
    uint32_t si=wi&0x3ffu; if(wi&0x400u)si=1023u-si;
    int neg=(wi&0x800u)!=0;
    int32_t ta=atten+(int32_t)s_sin[si];
    if(ta<0)ta=0;
    if(ta>=3840)return 0;
    int32_t out=(int32_t)(s_exp[(uint32_t)ta&255u] >> ((uint32_t)ta>>8));
    return neg?-out:out;
}

static inline int32_t synth_channel(LpYM2151 *e,int ch,int32_t pm,int32_t am)
{
    YMOp *p=&e->ops[ch*4];
    const int32_t mem=e->mem_value[ch];
    const int noise=(ch==7 && e->noise_enable);
    const int algo=e->algo[ch]&7;
    const int32_t o0=calc_op(e,&p[0],feedback(e,ch),pm,am,0);
    e->fb_memory[ch][0]=e->fb_memory[ch][1];
    e->fb_memory[ch][1]=o0;
    int32_t o1,o2,o3,out;
    switch(algo) {
    case 0: o1=calc_op(e,&p[1],mem,pm,am,0); o2=calc_op(e,&p[2],o0,pm,am,0); o3=calc_op(e,&p[3],o1,pm,am,noise); e->mem_value[ch]=o2; out=o3; break;
    case 1: o1=calc_op(e,&p[1],mem,pm,am,0); o2=calc_op(e,&p[2],0,pm,am,0); o3=calc_op(e,&p[3],o1,pm,am,noise); e->mem_value[ch]=o0+o2; out=o3; break;
    case 2: o1=calc_op(e,&p[1],mem,pm,am,0); o2=calc_op(e,&p[2],0,pm,am,0); o3=calc_op(e,&p[3],o0+o1,pm,am,noise); e->mem_value[ch]=o2; out=o3; break;
    case 3: o1=calc_op(e,&p[1],0,pm,am,0); o2=calc_op(e,&p[2],o0,pm,am,0); o3=calc_op(e,&p[3],mem+o1,pm,am,noise); e->mem_value[ch]=o2; out=o3; break;
    case 4: o1=calc_op(e,&p[1],0,pm,am,0); o2=calc_op(e,&p[2],o0,pm,am,0); o3=calc_op(e,&p[3],o1,pm,am,noise); out=o2+o3; break;
    case 5: o1=calc_op(e,&p[1],mem,pm,am,0); o2=calc_op(e,&p[2],o0,pm,am,0); o3=calc_op(e,&p[3],o0,pm,am,noise); e->mem_value[ch]=o0; out=o1+o2+o3; break;
    case 6: o1=calc_op(e,&p[1],0,pm,am,0); o2=calc_op(e,&p[2],o0,pm,am,0); o3=calc_op(e,&p[3],0,pm,am,noise); out=o1+o2+o3; break;
    default:o1=calc_op(e,&p[1],0,pm,am,0); o2=calc_op(e,&p[2],0,pm,am,0); o3=calc_op(e,&p[3],0,pm,am,noise); out=o0+o1+o2+o3; break;
    }
    return out;
}

static __attribute__((noinline)) void synth(LpYM2151 *e,int32_t *ml,int32_t *mr)
{
    int32_t l=0,r=0;
    const int32_t gpm=e->cached_pm;
    const int32_t gam=e->cached_am;
    const uint8_t active=e->active_ch_mask;
    for(int ch=0;ch<YM_CH;ch++) {
        if((active&(uint8_t)(1u<<ch))==0) continue;
        const uint8_t pms=e->pms[ch]&7u;
        const uint8_t ams=e->ams[ch]&3u;
        const int32_t pm=(gpm&&pms)?((gpm*s_pms_mult[pms])>>4):0;
        const int32_t am=(gam&&ams)?((gam*s_ams_mult[ams])>>2):0;
        const int32_t co=synth_channel(e,ch,pm,am);
        if(e->pan_l[ch]) l+=co;
        if(e->pan_r[ch]) r+=co;
    }
    *ml=l;*mr=r;
}

static inline void tick(LpYM2151 *e,int32_t *ol,int32_t *orr)
{
    if((e->output_tick_counter++&3u)==0u)update_envelopes(e);
    if(e->noise_enable) {
        e->noise_phase+=e->noise_step;
        if(!e->noise_step)e->noise_step=0x00800000u; /* doubled for 22.05 kHz */
        if(e->noise_phase<e->noise_step) {
            uint32_t bit=((e->noise_rng>>0)^(e->noise_rng>>3))&1u;
            e->noise_rng=(e->noise_rng>>1)|(bit<<16); if(!e->noise_rng)e->noise_rng=1;
        }
    }
    e->lfo_phase+=e->lfo_step;
    if(e->pmd||e->amd) {
        const uint8_t pos=(uint8_t)((e->lfo_phase>>24)&255u);
        e->cached_pm=e->pmd?((lfo_pm_value(e->lfo_wave,pos)*(int32_t)e->pmd+63)>>7):0;
        e->cached_am=e->amd?((lfo_am_value(e->lfo_wave,pos)*(int32_t)e->amd+63)>>7):0;
    } else { e->cached_pm=0; e->cached_am=0; }
    int32_t l=0,r=0;synth(e,&l,&r);
    l=(l+e->prev_l)/2;r=(r+e->prev_r)/2;e->prev_l=l;e->prev_r=r;
    *ol=l;*orr=r;
}

static void write_reg(uint8_t a,uint8_t d)
{
    LpYM2151 *e=&s_ym;
    if(a==0x01){if(d&2)e->lfo_phase=0;return;}
    if(a==0x08){int ch=d&7;for(int o=0;o<4;o++){YMOp *p=&e->ops[ch*4+o];if((d>>(3+o))&1){if(p->env_state==EG_OFF)p->env_level=EG_MAX;p->phase=0;p->env_state=EG_ATTACK;}else if(p->env_state!=EG_OFF)p->env_state=EG_RELEASE;}const YMOp *q=&e->ops[ch*4];if(q[0].env_state!=EG_OFF||q[1].env_state!=EG_OFF||q[2].env_state!=EG_OFF||q[3].env_state!=EG_OFF)e->active_ch_mask|=(uint8_t)(1u<<ch);else e->active_ch_mask&=(uint8_t)~(1u<<ch);return;}
    if(a==0x0f){e->noise_enable=(d>>7)&1;e->noise_step=s_noise_step[d&31u];return;}
    if(a==0x18){e->lfo_step=s_lfo_step[d];return;}
    if(a==0x19){if(d&0x80)e->amd=d&0x7f;else e->pmd=d&0x7f;return;}
    if(a==0x1b){e->lfo_wave=d&3;return;}
    if(a>=0x20&&a<=0x27){int ch=a&7;e->pan_l[ch]=(d>>7)&1;e->pan_r[ch]=(d>>6)&1;e->fb_shift[ch]=(d>>3)&7;e->algo[ch]=d&7;return;}
    if(a>=0x28&&a<=0x2f){int ch=a&7;e->kc[ch]=d;update_phase(e,ch);update_rates(e,ch);return;}
    if(a>=0x30&&a<=0x37){int ch=a&7;e->kf[ch]=d;update_phase(e,ch);return;}
    if(a>=0x38&&a<=0x3f){int ch=a&7;e->pms[ch]=(d>>4)&7;e->ams[ch]=d&3;return;}
    if(a>=0x40){int ch=a&7;int ot=(a>>3)&3;YMOp *p=&e->ops[ch*4+ot];switch(a&0xe0){
        case 0x40:p->mul=d&15;p->dt1=(d>>4)&7;update_phase(e,ch);break;
        case 0x60:p->tl_atten=(d&0x7f)*32;break;
        case 0x80:p->ks=d>>6;p->ar=d&31;update_rates(e,ch);break;
        case 0xa0:p->dr=d&31;p->am_enable=(d>>7)&1;update_rates(e,ch);break;
        case 0xc0:p->dt2=(d>>6)&3;p->d2r=d&31;update_phase(e,ch);update_rates(e,ch);break;
        case 0xe0:{int rr=d&15;p->rr=rr?(rr*2+1):0;int d1=(d>>4)&15;p->sl_level=(d1==15)?EG_MAX:(int32_t)(((uint32_t)d1*4u)*32u<<EG_FRACTION_BITS);update_rates(e,ch);break;}
    }}
}

static void csm_pulse(void)
{
    for(uint8_t ch=0;ch<8;ch++) { write_reg(0x08,ch); write_reg(0x08,(uint8_t)(ch|0x78u)); }
}

static inline void lp_shared_fence(void)
{
    __asm__ __volatile__("fence rw,rw" ::: "memory");
}

static inline uint32_t pack_sample(int32_t l,int32_t r)
{
    int32_t vol=s_ym.volume_q14;
    if(l>65535)l=65535;else if(l<-65536)l=-65536;
    if(r>65535)r=65535;else if(r<-65536)r=-65536;
    l=(l*vol)>>14;r=(r*vol)>>14;
    if(l>32767)l=32767;else if(l<-32768)l=-32768;
    if(r>32767)r=32767;else if(r<-32768)r=-32768;
    return (uint16_t)(int16_t)l | ((uint32_t)(uint16_t)(int16_t)r << 16);
}

static void render_44_domain(uint32_t frames44, uint8_t profile)
{
    uint32_t phase=lp_fm_half_phase & 1u;
    uint32_t total=phase + frames44;
    uint32_t low_frames=total >> 1;
    lp_fm_half_phase=total & 1u;
    if(!low_frames) return;

    uint32_t c0=lp_cycles();
    for(uint32_t i=0;i<low_frames;i++) {
        /* Never overwrite unread samples. Busy wait is intentional: producer
         * must preserve register/render order; HP audio consumer drains it. */
        while ((uint32_t)(lp_fm_out_head-lp_fm_out_tail) >= LP_FM_OUT_LEN) {
            ++lp_fm_out_stalls;
        }
        int32_t l,r; tick(&s_ym,&l,&r);
        lp_fm_out_ring[lp_fm_out_head & LP_FM_OUT_MASK]=pack_sample(l,r);
        lp_shared_fence();
        lp_fm_out_head = lp_fm_out_head + 1u;
        lp_shared_fence();
    }
    uint32_t dc=lp_cycles()-c0;
    uint32_t us=dc/40u; /* ESP32-P4 LP core nominal 40 MHz. */
    lp_fm_work_us += us;
    ++lp_fm_work_calls;
    lp_fm_work_frames44 += frames44;
    if(profile) {
        lp_fm_profile_us += us;
        ++lp_fm_profile_calls;
        lp_fm_profile_frames44 += frames44;
    }
}

static void process_event(uint32_t w0,uint32_t frames44)
{
    uint8_t type=(uint8_t)w0;
    uint8_t reg=(uint8_t)(w0>>8);
    uint8_t data=(uint8_t)(w0>>16);
    uint8_t profile=(uint8_t)(w0>>24);
    switch(type) {
    case LP_FM_WRITE:
        if(frames44) render_44_domain(frames44,profile);
        write_reg(reg,data);
        break;
    case LP_FM_RENDER:
        render_44_domain(frames44,profile);
        break;
    case LP_FM_RESET:
        reset_state();
        lp_fm_half_phase=0;
        lp_shared_fence();
        lp_fm_out_tail = lp_fm_out_head;
        lp_shared_fence();
        break;
    case LP_FM_VOLUME:
        s_ym.volume_q14=s_volume_q14[data<=16u?data:16u];
        break;
    case LP_FM_CSM:
        csm_pulse();
        break;
    default:
        break;
    }
}

int main(void)
{
    s_ym.volume_q14 = 16384;
    reset_state();
    lp_fm_cmd_head=lp_fm_cmd_tail=0;
    lp_fm_out_head=lp_fm_out_tail=0;
    lp_fm_cmd_drops=lp_fm_out_stalls=0;
    lp_fm_work_us=lp_fm_work_calls=lp_fm_work_frames44=0;
    lp_fm_profile_us=lp_fm_profile_calls=lp_fm_profile_frames44=0;
    lp_fm_heartbeat=1;
    lp_fm_half_phase=0;
    lp_fm_hp_probe=0;
    lp_fm_lp_echo=0;
    lp_shared_fence();
    lp_fm_ready = LP_FM_READY_MAGIC;
    lp_shared_fence();

    for(;;) {
        /* P4 LP RAM is shared RTC memory.  Use ordinary volatile accesses plus
         * explicit RISC-V fences instead of C atomics/AMOs for the SPSC ABI.
         * The probe/echo also proves live HP->LP visibility independently of
         * the queue indexes. */
        uint32_t probe = lp_fm_hp_probe;
        if(lp_fm_lp_echo != probe) {
            lp_shared_fence();
            lp_fm_lp_echo = probe;
            lp_shared_fence();
        }

        /* Build 6.15h10-exp: the raw probe/echo path is proven live on real
         * ESP32-P4 hardware, while the old cmd_head publication could remain
         * invisible (head=1/tail=0). Reuse hp_probe as the authoritative
         * producer doorbell after the startup magic handshake. HP first
         * returns it to zero, then publishes each new monotonic head here. */
        uint32_t tail=lp_fm_cmd_tail;
        lp_shared_fence();
        uint32_t head=probe;
        if(head==LP_FM_PROBE_MAGIC) {
            __asm__ __volatile__("nop");
            continue;
        }
        if(tail==head) {
            __asm__ __volatile__("nop");
            continue;
        }
        uint32_t idx=(tail & LP_FM_CMD_MASK)*2u;
        uint32_t w0=lp_fm_cmd_ring[idx];
        uint32_t w1=lp_fm_cmd_ring[idx+1u];
        lp_shared_fence();
        process_event(w0,w1);
        ++lp_fm_heartbeat;
        lp_shared_fence();
        lp_fm_cmd_tail=tail+1u;
        lp_shared_fence();
    }
}
