idf_component_register(SRCS "stub.c" INCLUDE_DIRS ".")

# Build 6.15h5-exp: this tiny *native ESP-IDF* helper project exists only to
# compile/link the ESP32-P4 LP-core firmware. PlatformIO's SCons bridge does
# not execute ulp_embed_binary()'s generated-file custom commands for LP-Core,
# so the main project imports the resulting binary as an ordinary C blob.
set(ulp_app_name px68k_lp_fm)
set(ulp_sources "ulp/lp_fm.c")
set(ulp_exp_dep_srcs "stub.c")
ulp_embed_binary(${ulp_app_name} "${ulp_sources}" "${ulp_exp_dep_srcs}")
