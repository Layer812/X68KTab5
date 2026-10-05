cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/src")

# X68K Tab keeps M5GFX pinned to the validated upstream commit and applies the
# small Tab5 panel-compatibility/DoubleFB overlay during configure. The script
# is exact-guarded and idempotent; it refuses an unexpected M5GFX revision.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_LIST_DIR}/patch_m5gfx_x68ktab.py"
    WORKING_DIRECTORY "${CMAKE_CURRENT_LIST_DIR}"
    RESULT_VARIABLE X68KTAB_M5GFX_PATCH_RC
    OUTPUT_VARIABLE X68KTAB_M5GFX_PATCH_OUT
    ERROR_VARIABLE X68KTAB_M5GFX_PATCH_ERR
)
if(NOT X68KTAB_M5GFX_PATCH_RC EQUAL 0)
    message(FATAL_ERROR
        "X68K Tab M5GFX patch failed:\n${X68KTAB_M5GFX_PATCH_OUT}\n${X68KTAB_M5GFX_PATCH_ERR}")
endif()
string(STRIP "${X68KTAB_M5GFX_PATCH_OUT}" X68KTAB_M5GFX_PATCH_OUT)
message(STATUS "${X68KTAB_M5GFX_PATCH_OUT}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# PX68K Tab5 native ESP-IDF 5.5.4 production build.
# Keep the proven emulator feature set fixed; all broad audit/profile instrumentation is removed from the R57E62 source tree.
# R57E68 RELEASE QUIET retains the proven R57E67 6ms phase reservoir,
# 3ms keep point, 20ms bounded catch-up credit, slack-charged prefetch and
# TEXT/audio arbitration. Focused Timer-B/PCM/mix runtime measurement is
# compiled out so the release candidate carries no periodic audit traffic.
# Release diagnostics are compiled out by default.
# Restore research telemetry only when needed by changing RELEASE_DIAGNOSTICS to 1.
# The historical wall benchmark is also kept recoverable but disabled in release.
#   -DPX68K_TAB5_RELEASE_DIAGNOSTICS=1
#   -DPX68K_TAB5_DYNAREC_PROD_BENCH=1
add_compile_options(
    -DBOARD_HAS_PSRAM
    -DPX68K_TAB5_RELEASE_DIAGNOSTICS=0
    -DPX68K_TAB5_R57E63_AUDIO_AUDIT=0
    -DPX68K_TAB5_DIAG_VERBOSE=0
    -DPX68K_TAB5_DIAG_GRAPHICS=0
    -DPX68K_TAB5_PERF_PROFILE=0
    -DPX68K_TAB5_DYNAREC=0
    -DPX68K_TAB5_DYNAREC_COST_PROFILE=0
    -DPX68K_TAB5_DYNAREC_PROD_BENCH=0
    -DPX68K_TAB5_R43_QUIET_RUNTIME=1
    -DPX68K_TAB5_DYNAREC_LOG=0
    -DPX68K_TAB5_INPUT_TRACE=0
    -DPX68K_TAB5_MIDI_TRACE=0
    -DPX68K_TAB5_AUDIO_RATE=44100
    -DPX68K_TAB5_AUDIO_VOLUME=33
    $<$<COMPILE_LANGUAGE:CXX>:-fpermissive>
)
add_link_options(-Wl,--enable-non-contiguous-regions)

project(px68k_tab5)
