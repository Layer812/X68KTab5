/*
 * PX68K Tab5 Build 5.80a: ESP32-P4 PIE/XespV MOVE.W stream backend.
 *
 * Compatibility follow-up to 5.80 for the ESP-IDF 5.4.2 / Espressif GCC
 * 14.2.0+20241119 assembler used by the Tab5 project.
 *
 * The 5.80 experiment used PIE SAR read/write mnemonics only to shift source
 * low bytes from even byte lanes to odd byte lanes.  That assembler accepts
 * the vector load/store/logical operations used here, but rejects those SAR
 * operand forms.  5.80a removes SAR state entirely:
 *
 *   - merge/copy vector backend is used only for the even-byte GVRAM page.
 *     The C eligibility layer rejects the odd-byte page and transparently uses
 *     the proven scalar 5.79 stream backend there.
 *   - repeat/fill vector backend supports BOTH pages without any shift: the
 *     broadcast byte is simply ANDed with the even or odd alternating mask.
 *
 * No guest-visible MOVE.W, scheduler, dirty-line, PC/A-register or CCR
 * semantics are changed by this compatibility adjustment.
 */
/*
 * Tab5 port-specific implementation.
 * Intent: ESP32-P4 XespV repeat helper for validated GVRAM stream operations; scalar behavior remains the fallback.
 * Layer8 Aug/17/2026
 */
    .section .iram1,"ax"
    .align 2
    .option norvc

    .global tab5_xespv_merge_wordblocks
    .type   tab5_xespv_merge_wordblocks, @function
    .balign 4
tab5_xespv_merge_wordblocks:
    beqz a2, .Lmerge_ret
    /* Odd-lane copies are filtered by the C eligibility layer.  Keep this
     * guard as a fail-safe rather than silently touching the wrong page. */
    bnez a3, .Lmerge_ret

    /* q2 = FF00FF00... (replace even bytes), q3 = inverse (preserve odd). */
    esp.vld.128.ip q2, a4, 0
    esp.notq       q3, q2
.Lmerge_even:
    esp.vld.128.ip q0, a1, 16
    esp.vld.128.ip q1, a0, 0
    esp.andq       q0, q0, q2
    esp.andq       q1, q1, q3
    esp.orq        q0, q0, q1
    esp.vst.128.ip q0, a0, 16
    addi a2, a2, -1
    bnez a2, .Lmerge_even
.Lmerge_ret:
    ret
    .size tab5_xespv_merge_wordblocks, .-tab5_xespv_merge_wordblocks


    .global tab5_xespv_fill_wordblocks
    .type   tab5_xespv_fill_wordblocks, @function
    .balign 4
tab5_xespv_fill_wordblocks:
    beqz a2, .Lfill_ret

    /* q2 = even-byte mask, q3 = odd-byte mask. */
    esp.vld.128.ip q2, a4, 0
    esp.notq       q3, q2
    /* Broadcast the low byte once per helper call. */
    esp.vldbc.8.ip q0, a1, 0
    bnez a3, .Lfill_odd_setup

    /* Replace even bytes, preserve odd bytes. */
    esp.andq       q0, q0, q2
.Lfill_even:
    esp.vld.128.ip q1, a0, 0
    esp.andq       q1, q1, q3
    esp.orq        q4, q0, q1
    esp.vst.128.ip q4, a0, 16
    addi a2, a2, -1
    bnez a2, .Lfill_even
.Lfill_ret:
    ret

.Lfill_odd_setup:
    /* No SAR/shift needed: broadcast has the value in every byte lane. */
    esp.andq       q0, q0, q3
.Lfill_odd:
    esp.vld.128.ip q1, a0, 0
    esp.andq       q1, q1, q2
    esp.orq        q4, q0, q1
    esp.vst.128.ip q4, a0, 16
    addi a2, a2, -1
    bnez a2, .Lfill_odd
    ret
    .size tab5_xespv_fill_wordblocks, .-tab5_xespv_fill_wordblocks


    /*
     * Build 5.98g5: ordinary-RAM zero-fill block primitive.
     * Uses only PIE forms already proven by the graphics FILL8 path.
     * a0=dst (16-byte aligned), a1=&zero_byte, a2=number of 16-byte blocks.
     */
    .global tab5_xespv_zero_byteblocks
    .type   tab5_xespv_zero_byteblocks, @function
    .balign 4
tab5_xespv_zero_byteblocks:
    beqz a2, .Lzero_ret
    esp.vldbc.8.ip q0, a1, 0
.Lzero_loop:
    esp.vst.128.ip q0, a0, 16
    addi a2, a2, -1
    bnez a2, .Lzero_loop
.Lzero_ret:
    ret
    .size tab5_xespv_zero_byteblocks, .-tab5_xespv_zero_byteblocks
