#pragma once
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TAB5_EP2_START_FRAME 900u
#define TAB5_EP2_CAPTURE_FRAMES 192u
#define TAB5_EP2_SAMPLE_PERIOD 128u

enum {
    TAB5_FR_MAIN_PRE=0, TAB5_FR_MAIN_EXEC, TAB5_FR_MAIN_FLUSH, TAB5_FR_MAIN_POST,
    TAB5_FR_LIB_MACHINE, TAB5_FR_LIB_DRAWLINE, TAB5_FR_LIB_FINISH,
    TAB5_FR_LIB_DSEND, TAB5_FR_LIB_FLUSH, TAB5_FR_LIB_FRAMEEND, TAB5_FR_LIB_WINDRAW,
    TAB5_FR_MK_CHOOSE, TAB5_FR_MK_M68K, TAB5_FR_MK_MFP, TAB5_FR_MK_RTC, TAB5_FR_MK_DMA,
    TAB5_FR_MK_POSTLINE, TAB5_FR_MK_ADPCM, TAB5_FR_MK_OPM, TAB5_FR_MK_MIDI, TAB5_FR_MK_INPUT,
    TAB5_FR_R57_PUBLISH, TAB5_FR_AUDIO_PUBLISH, TAB5_FR_COMPOSE_PUBLISH,
    TAB5_FR_PHASE_COUNT
};

enum {
    TAB5_EP_PATH_MDX622_ZERO=0,
    TAB5_EP_PATH_MDX619_FIXED,
    TAB5_EP_PATH_M2_EARLY,
    TAB5_EP_PATH_M2_FALLBACK,
    TAB5_EP_PATH_S2,
    TAB5_EP_PATH_DIRECT,
    TAB5_EP_PATH_CORE_FAST,
    TAB5_EP_PATH_BNE_FAST,
    TAB5_EP_PATH_BEQ_FAST,
    TAB5_EP_PATH_BCC_FAST,
    TAB5_EP_PATH_DBF_FAST,
    TAB5_EP_PATH_POST_OTHER,
    TAB5_EP_PATH_OTHER,
    TAB5_EP_PATH_COUNT
};

#define TAB5_EP2_RF_SAME_PAGE   0x01u
#define TAB5_EP2_RF_DECODE_NOW  0x02u
#define TAB5_EP2_RF_XOP_NULL    0x04u
#define TAB5_EP2_RF_M2_EARLY    0x08u
#define TAB5_EP2_RF_FUSED       0x10u

extern volatile uint32_t g_tab5_cpu1_fr_active;
extern volatile uint32_t g_tab5_cpu1_fr_guest_tick;
extern volatile uint32_t g_tab5_cpu1_ep_active;
extern volatile uint32_t g_tab5_cpu1_ep_sample_counter;

int  tab5_cpu1_fr_suppress_s5(uint32_t frame);
void tab5_cpu1_fr_frame_begin(uint32_t frame);
void tab5_cpu1_fr_frame_wall_start(int64_t wall_us);
void tab5_cpu1_fr_frame_end(uint32_t frame, int64_t wall_us);
void tab5_cpu1_fr_add_phase(uint32_t phase, uint32_t cycles);
void tab5_cpu1_fr_note_slice(uint32_t pc0,uint32_t pc1,int request,int executed,uint32_t reason,
                             uint32_t choose_cyc,uint32_t exec_cyc,uint32_t mfp_cyc,uint32_t rtc_cyc,uint32_t dma_cyc);
void tab5_cpu1_fr_note_finish(uint32_t adpcm,uint32_t opm,uint32_t midi,uint32_t input);
void tab5_cpu1_fr_note_poll(uint32_t kind,uint32_t pc,uint32_t guest_cycles);
void tab5_cpu1_fr_note_mmio(uint32_t pc,uint32_t addr,uint8_t width,uint8_t rw);
void tab5_cpu1_fr_audio_tick(uint32_t tick);
void tab5_cpu1_fr_note_audio_push(uint8_t type,uint32_t tick,uint32_t seq,uint32_t depth,uint32_t pub_cyc,uint32_t notify_cyc);
void tab5_cpu1_fr_note_r57(uint32_t domain,uint32_t seq,uint32_t depth,uint32_t reason,uint32_t pub_cyc,uint32_t notify_cyc);
void tab5_cpu1_fr_note_compose(uint32_t kind,uint32_t seq,uint32_t depth,uint32_t pub_cyc,uint32_t notify_cyc);
void tab5_cpu1_fr_note_framepub(uint32_t kind,uint32_t cycles);
void tab5_cpu1_fr_note_wait(uint32_t kind,uint32_t us);
void tab5_cpu1_fr_dump_once(void);

void tab5_cpu1_ep2_sample_record(uint16_t op,uint8_t path,uint8_t rflags,
                                 uint32_t total,uint32_t front,
                                 uint32_t pre,uint32_t gate,uint32_t addr,
                                 uint32_t resolve,uint32_t unpack);

#ifdef __cplusplus
}
#endif
