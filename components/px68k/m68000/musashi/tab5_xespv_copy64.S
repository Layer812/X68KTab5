/*
 * PX68K Tab5 Build 5.57: ESP32-P4 XespV 64-byte Musashi D/A snapshot.
 * The m68ki_cpu object is 16-byte aligned, while dar and dar_save are each
 * at +4 mod 16 in the original Musashi context layout.  Preserve that layout
 * (and save-state context size) by copying 12 bytes scalar, 48 bytes vector,
 * then the final 4 bytes scalar.
 */
/*
 * Tab5 port-specific implementation.
 * Intent: ESP32-P4 XespV helper used only after exact startup validation; scalar code remains the compatibility fallback.
 * Layer8 Aug/17/2026
 */
    .section .iram1,"ax"
    .align 2
    .global tab5_xespv_copy64
    .type tab5_xespv_copy64, @function
    .balign 4
    .option norvc

tab5_xespv_copy64:
    lw t0, 0(a1)
    sw t0, 0(a0)
    lw t0, 4(a1)
    sw t0, 4(a0)
    lw t0, 8(a1)
    sw t0, 8(a0)
    addi a1, a1, 12
    addi a0, a0, 12
    /* Match Espressif's aligned memcpy cadence: two vectors are loaded and
     * immediately stored before reusing q0.  This avoids relying on a three-Q
     * outstanding-load sequence on P4 revisions/toolchains. */
    esp.vld.128.ip q0, a1, 16
    esp.vld.128.ip q1, a1, 16
    esp.vst.128.ip q0, a0, 16
    esp.vst.128.ip q1, a0, 16
    esp.vld.128.ip q0, a1, 16
    esp.vst.128.ip q0, a0, 16
    lw t0, 0(a1)
    sw t0, 0(a0)
    ret
    .size tab5_xespv_copy64, .-tab5_xespv_copy64
