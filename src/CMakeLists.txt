idf_component_register(
    SRCS
        "main.c"
        "tab5_cpu_clock.c"
        "tab5_dynarec_arena.c"
        "tab5_video.cpp"
        "tab5_ppa.c"
        "tab5_px68k_hotmem.c"
        "tab5_launcher.cpp"
        "tab5_media_ui.cpp"
        "tab5_panic.cpp"
        "panic_x_bin.c"
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
        esp_driver_gpio
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


# R57E69 Native Profile Bridge
# The profile values are supplied by scripts/native_build_from_platformio.py.
# sdkconfig intentionally remains at the validated ESP32-P4 360 MHz boot setup;
# Turbo400 performs the HP-core switch at app_main via tab5_cpu_clock.c.
set(PX68K_TAB5_P4_TURBO400 "0" CACHE STRING "R57E69 experimental P4 400 MHz runtime profile")
set(PX68K_TAB5_HOST_CPU_MHZ "360" CACHE STRING "R57E69 measured host CPU profile")
target_compile_definitions(${COMPONENT_LIB} PRIVATE
    PX68K_TAB5_P4_TURBO400=${PX68K_TAB5_P4_TURBO400}
    PX68K_TAB5_HOST_CPU_MHZ=${PX68K_TAB5_HOST_CPU_MHZ}
)


# BAT146 LPFAB R4: R3 DIRTY retained + independent 1/1024 EXACT hash certification; R57 production unchanged
set(ulp_app_name ulp_px68k_lpbr)
set(ulp_sources "ulp/lp_broker.c")
set(ulp_exp_dep_srcs "tab5_lp_broker.c")
ulp_embed_binary(${ulp_app_name} "${ulp_sources}" "${ulp_exp_dep_srcs}")
