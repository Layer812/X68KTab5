idf_component_register(
    SRCS
        "main.c"
        "retrop4_midi_uart.cpp"
        "midmod/midi_engine.c"
        "midmod/gs2sam.c"
        "midmod/gs2sam_processor.c"
        "midmod/gs2sam_profile_sc55mk2.c"
        "midmod/gs2sam_profile_csv.c"
        "tab5_dynarec_arena.c"
        "tab5_video.cpp"
        "tab5_ppa.c"
        "tab5_px68k_hotmem.c"
        "tab5_launcher.cpp"
        "tab5_media_ui.cpp"
        "tab5_audio.cpp"
        "tab5_sd.c"
        "tab5_textview.c"
        "tab5_guest_input.c"
        "tab5_usb_keymap.c"
        "tab5_usb_keyboard.c"
        "tab5_disk_control.c"
        "tab5_compose.c"
        "tab5_guest_bus.c"
        "tab5_lp_broker.c"
        "tab5_screen_manager.c"
    INCLUDE_DIRS
        "."
        "midmod"
    REQUIRES
        # Project/core
        px68k

        # M5Stack display/audio/input wrappers used directly by src/*.cpp
        M5Unified
        M5GFX

        # ESP-IDF headers used directly by src/*
        esp_common
        freertos
        heap
        log
        esp_timer
        esp_hw_support
        esp_driver_gpio esp_driver_uart
        ulp

        # Storage
        fatfs
        esp_driver_sdmmc
        sdmmc

        # USB HID host
        usb
        usb_host_hid

        # Display / acceleration / memory
        esp_lcd
        esp_driver_ppa
        esp_mm

        # C library headers used by the src component
        newlib
)

# PX68K_NATIVE55_WARNING_BRIDGE_V24
# Diagnostics remain visible; only promotion to errors is disabled for known migration warnings.
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=stringop-truncation -Wno-error=format-truncation -Wno-error=maybe-uninitialized -Wno-error=multistatement-macros -Wno-error=shift-count-overflow -Wno-error=overflow -Wno-error=deprecated-declarations -Wno-error=discarded-qualifiers -Wno-error=incompatible-pointer-types -Wno-error=int-conversion)


# P12R1 product: ESP32-P4 host CPU remains at the validated ESP-IDF 360 MHz profile.
# The retired 400 MHz runtime experiment has no build option or code path.


# R139A6A3: LP TouchJoy latest/control transport only; mirrored-R57 observer retired
set(ulp_app_name ulp_px68k_lpbr)
set(ulp_sources "ulp/lp_broker.c")
set(ulp_exp_dep_srcs "tab5_lp_broker.c")
ulp_embed_binary(${ulp_app_name} "${ulp_sources}" "${ulp_exp_dep_srcs}")
