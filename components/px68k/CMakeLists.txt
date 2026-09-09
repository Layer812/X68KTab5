set(PX68K_C_SOURCES
    # ------------------------------------------------------------
    # 68000 wrapper
    # ------------------------------------------------------------
    m68000/m68000.c

    # ------------------------------------------------------------
    # Musashi
    # ------------------------------------------------------------
    m68000/musashi/m68kcpu.c
    m68000/musashi/m68kops.c
    m68000/musashi/softfloat/softfloat.c

    # ------------------------------------------------------------
    # X68000 hardware
    # ------------------------------------------------------------
    x68k/adpcm.c
    x68k/bg.c
    x68k/crtc.c
    x68k/dmac.c
    x68k/fdc.c
    x68k/fdd.c
    x68k/disk_d88.c
    x68k/disk_dim.c
    x68k/disk_xdf.c
    x68k/gvram.c
    x68k/hostfs.c
    x68k/tab5_xespv_stream.S
    x68k/ioc.c
    x68k/irqh.c
    x68k/mem_wrap.c
    x68k/mercury.c
    x68k/mfp.c
    x68k/palette.c
    x68k/midi.c
    x68k/pia.c
    x68k/rtc.c
    x68k/sasi.c
    x68k/scc.c
    x68k/scsi.c
    x68k/sram.c
    x68k/sysport.c
    x68k/tvram.c

    # ------------------------------------------------------------
    # Original PX68K host/libretro compatibility layer
    # Keep these for Build 0.
    # ------------------------------------------------------------
    libretro/joystick.c
    libretro/keyboard.c
    libretro/mouse.c
    libretro/prop.c
    libretro/timer.c
    libretro/dswin.c
    libretro/windraw.c
    libretro/tab5_video_cpu1.c
    libretro/tab5_video_flow.c
    libretro/tab5_guest_video_state.c
    libretro/winui.c
    libretro/dosio.c
    libretro/fake.c
    libretro/peace.c
    libretro/state.c

    # ------------------------------------------------------------
    # Main libretro PX68K glue
    # ------------------------------------------------------------
    libretro.c

    iplromxv_bin.c
    cgrom_bin.c
)

set(PX68K_CXX_SOURCES
    fmgen/fmg_wrap.cpp
    fmgen/vgmm5_ym2151.cpp
    fmgen/fmgen.cpp
    fmgen/fmtimer.cpp
    fmgen/opm.cpp
    fmgen/opna.cpp
    fmgen/psg.cpp
)

idf_component_register(
    SRCS
        ${PX68K_C_SOURCES}
        ${PX68K_CXX_SOURCES}

    INCLUDE_DIRS
        .
        x68k
        fmgen
        libretro
        m68000
        m68000/musashi
        m68000/musashi/softfloat
        libretro-common/include

    PRIV_REQUIRES
        esp_psram
        esp_hw_support
        esp_partition

    LDFRAGMENTS
        "px68k_ext_ram.lf"

    WHOLE_ARCHIVE
    PRIV_REQUIRES\n        esp_timer\n\n)
if(NOT CONFIG_SPIRAM)
    message(FATAL_ERROR
        "PX68K: CONFIG_SPIRAM is OFF - PSRAM is required")
endif()

if(NOT CONFIG_SPIRAM_BOOT_INIT)
    message(FATAL_ERROR
        "PX68K: CONFIG_SPIRAM_BOOT_INIT is OFF")
endif()

if(NOT CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY)
    message(FATAL_ERROR
        "PX68K: CONFIG_SPIRAM_ALLOW_BSS_SEG_EXTERNAL_MEMORY is OFF")
endif()

message(STATUS "PX68K: PSRAM enabled")
message(STATUS "PX68K: external BSS enabled")

# PX68K Makefile.common øΩÃïWøΩøΩ Musashi øΩ\øΩøΩ
target_compile_definitions(
    ${COMPONENT_LIB}
    PRIVATE
        HAVE_MUSASHI=1
)

# øΩ√ÇÔøΩC++øΩRøΩ[øΩhøΩøΩESP-IDF/GCC 14øΩ≈Ç‹ÇÔøΩøΩ ÇÔøΩøΩøΩøΩﬂÅB
# øΩKøΩvøΩ»â”èÔøΩøΩøΩøΩøΩøΩøΩ≈êÔøΩøΩÌâªøΩøΩøΩøΩB
target_compile_options(
    ${COMPONENT_LIB}
    PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
)

# PX68K_NATIVE55_WARNING_BRIDGE_V24
# Diagnostics remain visible; only promotion to errors is disabled for known migration warnings.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=stringop-truncation -Wno-error=format-truncation -Wno-error=maybe-uninitialized -Wno-error=multistatement-macros -Wno-error=shift-count-overflow -Wno-error=overflow -Wno-error=deprecated-declarations -Wno-error=discarded-qualifiers -Wno-error=incompatible-pointer-types -Wno-error=int-conversion)


# P12R1 Production Quiet: suppress raw PX68K stdout diagnostics at compile time.
target_compile_options(${COMPONENT_LIB} PRIVATE "-include${CMAKE_CURRENT_LIST_DIR}/product_quiet.h")
