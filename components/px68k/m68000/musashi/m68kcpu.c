/* ======================================================================== */
/* ========================= LICENSING & COPYRIGHT ======================== */
/* ======================================================================== */
/*
 *                                  MUSASHI
 *                                Version 4.60
 *
 * A portable Motorola M680x0 processor emulation engine.
 * Copyright Karl Stenerud.  All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.

 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */


/* ======================================================================== */
/* ================================= NOTES ================================ */
/* ======================================================================== */



/*
 * PX68K source modified for the Tab5 port.
 * Intent: ESP32-P4 CPU1 acceleration: cache dispatch metadata and batch only exact, scheduler-bounded instruction patterns while preserving Musashi-visible results and exit semantics.
 * Layer8 Aug/17/2026
 */
/* ======================================================================== */
/* ================================ INCLUDES ============================== */
/* ======================================================================== */

extern void m68040_fpu_op0(void);
extern void m68040_fpu_op1(void);
extern void m68881_mmu_ops(void);
extern unsigned char m68ki_cycles[][0x10000];
extern void (*m68ki_instruction_jump_table[0x10000])(void); /* opcode handler jump table */
extern void m68ki_build_opcode_table(void);

#include "m68kops.h"
#include "m68kcpu.h"

#include "m68kfpu.c"
#include "m68kmmu.h" /* uses some functions from m68kfpu.c which are static ! */

#ifdef ESP_PLATFORM
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "esp_attr.h"
#include <stddef.h>
#include <stdlib.h>
#include "../../x68k/x68kmemory.h"
#include "../../x68k/tvram.h"
#include "../../x68k/mfp.h"
#include "../../x68k/rtc.h"
#include "../../x68k/dmac.h"
#include "../../x68k/crtc.h"
#include "../../x68k/fdd.h"
#include "../../x68k/scc.h"
#include "../../x68k/adpcm.h"
#include "../../x68k/midi.h"
#include "../../fmgen/fmg_wrap.h"
#include "../../libretro/keyboard.h"

/* R57E138A: X68P4 processor cutover.  Runtime RV32 generation, generic JIT
 * discovery and the small/sparse Trace134-137 entry paths are retired.  The
 * hot executor consumes predecoded X68P4Op pages from fixed Internal SRAM. */
#ifdef PX68K_TAB5_DYNAREC
#undef PX68K_TAB5_DYNAREC
#endif
#define PX68K_TAB5_DYNAREC 0
#define PX68K_TAB5_DYNAREC_GENERIC 0
#define PX68K_TAB5_TRACE134 0
#define PX68K_TAB5_X68P4_PREDECODE 1

/* R140N2R6 SAFELEAN: unreachable Musashi host bus-error rollback is physically removed.
 * PX68K BusErrFlag/BusErrHandling is not wired to this upstream setjmp/longjmp path.
 * Address Error handling is unchanged. */

/* Build 6.15h17R27 A/B: disable only the R22 ZERO-RUN outer batching call.
 * Keep the implementation and its IRAM/data footprint linked so R26a vs R27
 * compares execution behavior rather than changing the memory layout. */
#ifndef PX68K_TAB5_MDX622_ZERO_RUN_AB
#define PX68K_TAB5_MDX622_ZERO_RUN_AB 1
#endif

/* R139A6A2 production-clean: dead ESP bus-error rollback snapshot/XespV self-check physically removed. */

/* R139A6 production-clean: historical opcode/hot-PC/backedge profiler
 * storage and observation machinery physically removed. */
uint32_t m68k_tab5_profile_storage_bytes(void) { return 0u; }

/* R139A6 production-clean: retired generic runtime dynarec physically removed.
 * X68P4 predecode is the only ESP execution-cache architecture. */
unsigned int m68k_tab5_dynarec_metadata_bytes(void) { return 0u; }
void m68k_tab5_dynarec_bind(void *arena, unsigned int bytes,
                            int (*sync_fn)(void *addr, unsigned int bytes))
{ (void)arena; (void)bytes; (void)sync_fn; }

/* R57E138A link-compatible tombstones: runtime trace/JIT code is not built. */
void m68k_tab5_trace134_bind(void *arena, unsigned int bytes,
                             int (*sync_fn)(void *addr, unsigned int bytes))
{ (void)arena; (void)bytes; (void)sync_fn; }
void m68k_tab5_trace134_invalidate_all(void) { }
void m68k_tab5_trace134_invalidate_range(uint32_t address, uint32_t bytes)
{ (void)address; (void)bytes; }


#ifdef ESP_PLATFORM
const unsigned int x68p4_r140n2r6_safelean_cpu_marker = 0x14020601u;
#endif

/* profile-safe code peek */
static inline __attribute__((always_inline)) uint16_t tab5_poll58_fetch16(uint32_t a);

/* R139A6 production-clean: retired sparse PIE/MOVEW observation studies
 * physically removed; accelerator semantics below are unchanged. */

/*
 * Build 5.59 (ESP32-P4): retain the 256-entry zero-wait opcode-dispatch cache.
 *
 * The full Musashi handler table (64K pointers) and the 64K cycle row live in
 * external BSS on Tab5.  A normal instruction therefore performs two nearly
 * random PSRAM-backed metadata reads after the opcode itself has been fetched:
 *     handler = m68ki_instruction_jump_table[opcode]
 *     cycles  = CYC_INSTRUCTION[opcode]
 * Keep a small direct-mapped working set in P4 TCM/SPM instead.  Misses still
 * use the authoritative Musashi tables, so this changes no emulation state.
 */
/* Intent: Keep the hot opcode handler/metadata working set in TCM/internal DRAM so CPU1 does not pay PSRAM latency on every guest instruction.  Layer8 Aug/17/2026 */
/* R57E138A: the old opcode dispatch L1/L2 are no longer in the execute path.
 * Keep one-entry sentinels only for archived diagnostics/API compatibility. */
#define TAB5_M68K_DISPATCH_CACHE_BITS 0u
#define TAB5_M68K_DISPATCH_CACHE_SIZE 1u
#define TAB5_M68K_DISPATCH_CACHE_MASK 0u
#define TAB5_M68K_DISPATCH_L2_BITS 0u
#define TAB5_M68K_DISPATCH_L2_SIZE 1u
#define TAB5_M68K_DISPATCH_L2_MASK 0u

typedef void (*tab5_m68k_handler_t)(void);

/* Build 5.87: fuse the three normal post-instruction opcode classifiers into
 * the dispatch-cache metadata.  Bits 30:24 were unused in sig_cycles, so a
 * cache hit can now tell us whether any rare post-op accelerator is relevant
 * with one branch instead of re-testing stream/poll/DBF patterns every guest
 * instruction.  The accelerator implementations themselves are unchanged. */
#define TAB5_POST585_STREAM        0x01u
#define TAB5_POST585_POLL          0x02u
#define TAB5_POST585_DBF           0x04u
#define TAB5_POST598F_BNE_FAST     0x08u
#define TAB5_POST598F_BEQ_FAST     0x10u
#define TAB5_POST598F_BCC_FAST     0x20u
#define TAB5_POST598F_CORE_FAST    0x40u

static inline __attribute__((always_inline)) uint32_t tab5_post585_flags(uint16_t op)
{
    uint32_t f = 0;
    const uint16_t cls = op & 0xf1f8u;
    if (cls == 0x30c0u || cls == 0x30d8u)
        f |= TAB5_POST585_STREAM;
    if (op == 0x66f4u || op == 0x66f6u || op == 0x65f6u ||
        op == 0x66f8u || op == 0x67f8u ||
        op == 0x66fcu || op == 0x67fcu)
        f |= TAB5_POST585_POLL;
    /* Build 5.98f: tag all DBcc forms so their small architectural core can
     * execute before the indirect Musashi handler.  Existing DBF batching is
     * still attempted only for DBF after the exact first iteration. */
    if ((op & 0xf0f8u) == 0x50c8u)
        f |= TAB5_POST585_DBF;

    /* Build 5.98f: extend the proven 5.98e short-Bcc path to the .W forms.
     * BRA/BSR remain authoritative because they have distinct side effects. */
    if ((op & 0xf000u) == 0x6000u && ((op >> 8) & 0x0fu) >= 2u) {
        if ((op & 0xff00u) == 0x6600u)
            f |= TAB5_POST598F_BNE_FAST;
        else if ((op & 0xff00u) == 0x6700u)
            f |= TAB5_POST598F_BEQ_FAST;
        else
            f |= TAB5_POST598F_BCC_FAST;
    }

    /* Build 5.98f CORE_FAST: exact register-only kernels plus TST.W (An).
     * These are the concrete op families observed at the top of the SFXVI
     * profiles.  No memory/MMIO caching is introduced. */
    if ((op & 0xfff8u) == 0x4a50u || /* TST.W (An) */
        (op & 0xfff8u) == 0x4a40u || /* TST.W Dn */
        (op & 0xfff8u) == 0x4840u || /* SWAP Dn */
        (op & 0xf100u) == 0x7000u || /* MOVEQ */
        (op & 0xf1f8u) == 0x1000u || /* Build 5.98g7: MOVE.B Dn,Dm */
        (op & 0xf1f8u) == 0x3000u || /* MOVE.W Dn,Dm */
        (op & 0xf1f8u) == 0x30c0u || /* Build 5.98g7: MOVE.W Dn,(An)+ */
        (op & 0xf1f8u) == 0x20c0u || /* Build 5.98g9: MOVE.L Dn,(An)+ */
        (op & 0xf1f8u) == 0x20d8u || /* Build 5.98g10: MOVE.L (An)+,(Am)+ */
        (op & 0xfff8u) == 0x0800u || /* Build 5.98g10: BTST #imm,Dn */
        (op & 0xfff8u) == 0x4298u || /* Build 5.98g10: CLR.L (An)+ */
        (op & 0xfff8u) == 0x48c0u || /* Build 5.98g10: EXT.L Dn */
        (op & 0xfff8u) == 0x4258u || /* Build 5.98g9: CLR.W (An)+ */
        (op & 0xf1f8u) == 0x5048u || /* Build 5.98g9: ADDQ.W #n,An */
        (op & 0xf1f8u) == 0x5088u || /* Build 5.98g9: ADDQ.L #n,An */
        (op & 0xfff8u) == 0x48e0u || /* Build 5.98g9: MOVEM.L regs,-(An) */
        (op & 0xfff8u) == 0x4cd8u || /* Build 5.98g9: MOVEM.L (An)+,regs */
        (op & 0xf1f8u) == 0x2000u || /* MOVE.L Dn,Dm */
        (op & 0xf1f8u) == 0xd040u || /* ADD.W Dn,Dm */
        (op & 0xf1f8u) == 0xc040u || /* AND.W Dn,Dm */
        (op & 0xf1ffu) == 0xc07cu || /* AND.W #imm,Dn */
        (op & 0xf1ffu) == 0xc0bcu || /* AND.L #imm,Dn */
        (op & 0xfff8u) == 0x0240u || /* ANDI.W #imm,Dn */
        (op & 0xfff8u) == 0x0280u || /* ANDI.L #imm,Dn */
        (op & 0xf1f8u) == 0x5040u || /* ADDQ.W #n,Dn */
        (op & 0xf1f8u) == 0x5140u || /* SUBQ.W #n,Dn */
        (op & 0xf1f8u) == 0xd168u || /* R129A: ADD.W Dn,d16(An) measured hot */
        (op & 0xf1f8u) == 0xe048u || /* R129A: LSR.W #n,Dn measured hot */
        (op & 0xf1f8u) == 0x1010u || /* R130A: MOVE.B (An),Dn measured hot */
        (op & 0xf1f8u) == 0xd068u || /* R130A: ADD.W d16(An),Dn measured hot */
        (op & 0xfff8u) == 0x0c40u || /* R130A: CMPI.W #imm,Dn measured hot */
        (op & 0xfff8u) == 0x4a00u || /* R130A: TST.B Dn measured hot */
        (op & 0xf1f8u) == 0xe040u || /* ASR.W #n,Dn */
        (op & 0xf1f8u) == 0xe080u)   /* ASR.L #n,Dn */
        f |= TAB5_POST598F_CORE_FAST;
    return f;
}

static inline __attribute__((always_inline)) int tab5_bcc598f_taken(uint16_t op)
{
    switch ((op >> 8) & 0x0fu) {
    case 0x2: return COND_HI() != 0;
    case 0x3: return COND_LS() != 0;
    case 0x4: return COND_CC() != 0;
    case 0x5: return COND_CS() != 0;
    case 0x6: return COND_NE() != 0;
    case 0x7: return COND_EQ() != 0;
    case 0x8: return COND_VC() != 0;
    case 0x9: return COND_VS() != 0;
    case 0xa: return COND_PL() != 0;
    case 0xb: return COND_MI() != 0;
    case 0xc: return COND_GE() != 0;
    case 0xd: return COND_LT() != 0;
    case 0xe: return COND_GT() != 0;
    case 0xf: return COND_LE() != 0;
    default:  return 0;
    }
}

/* Build 5.98f: execute Bcc.B/Bcc.W core exactly as generated Musashi does.
 * The base instruction cycles are charged by the caller; this helper charges
 * only the not-taken addend and consumes the .W extension exactly once. */
static inline __attribute__((always_inline)) void tab5_bcc598f_exec(uint16_t op, int taken)
{
    if ((op & 0x00ffu) != 0u) {
        if (taken) {
            m68ki_trace_t0();
            REG_PC += MAKE_INT_8(op & 0x00ffu);
        } else {
            USE_CYCLES(CYC_BCC_NOTAKE_B);
        }
        return;
    }

    if (taken) {
        const uint offset = OPER_I_16();
        REG_PC -= 2;
        m68ki_trace_t0();
        m68ki_branch_16(offset);
    } else {
        REG_PC += 2;
        USE_CYCLES(CYC_BCC_NOTAKE_W);
    }
}

/* Build 5.98f: DBcc core.  Returns nonzero if the branch was taken.
 * This mirrors the generated Musashi handlers, including extension fetch,
 * decrement timing, trace behavior and the DBF no-exp/exp cycle addends. */
static inline __attribute__((always_inline)) int tab5_dbcc598f_exec(uint16_t op)
{
    const unsigned cc = (op >> 8) & 0x0fu;
    const int cond_true = (cc == 0u) ? 1 : (cc == 1u) ? 0 : tab5_bcc598f_taken(op);

    if (cond_true) {
        REG_PC += 2;
        return 0;
    }

    {
        uint * const r_dst = &REG_D[op & 7u];
        const uint res = MASK_OUT_ABOVE_16(*r_dst - 1u);
        *r_dst = MASK_OUT_BELOW_16(*r_dst) | res;
        if (res != 0xffffu) {
            const uint offset = OPER_I_16();
            REG_PC -= 2;
            m68ki_trace_t0();
            m68ki_branch_16(offset);
            USE_CYCLES(CYC_DBCC_F_NOEXP);
            return 1;
        }
        REG_PC += 2;
        USE_CYCLES(CYC_DBCC_F_EXP);
        return 0;
    }
}

/* Build 5.98f: exact handler-call elimination for register-only hot families.
 * The implementation is intentionally copied from the corresponding generated
 * Musashi handlers.  X is preserved for MOVE/TST/AND/SWAP and updated only by
 * arithmetic/shift instructions, exactly like stock Musashi. */
static inline __attribute__((always_inline)) void tab5_core598f_exec(uint16_t op)
{
    if ((op & 0xf1f8u) == 0xd168u) { /* R57E129A: ADD.W Dn,d16(An) */
        /* The generated handler performs two top-level data-space traversals
         * for the same EA (read then write).  This measured-hot family can
         * classify the EA once.  Current PX68K production has FC/PMMU off;
         * odd/MMIO/unusual addresses stay on the authoritative wrappers. */
        const uint ea = AY + MAKE_INT_16(m68ki_read_imm_16());
        const uint src = MASK_OUT_ABOVE_16(DX);
#ifdef ESP_PLATFORM
        const uint32_t bus = (uint32_t)ADDRESS_68K(ea);
        if (__builtin_expect(((ea & 1u) == 0u) && bus <= 0x00bffffeu, 1)) {
            const uint dst = (uint)tab5_data584_native_word_load(MEM + bus);
            const uint res = src + dst;
            const uint out = MASK_OUT_ABOVE_16(res);
            BusErrFlag = 0;
            FLAG_N = NFLAG_16(res);
            FLAG_V = VFLAG_ADD_16(src, dst, res);
            FLAG_X = FLAG_C = CFLAG_16(res);
            FLAG_Z = out;
            tab5_data584_native_word_store(MEM + bus, out);
            m68k_tab5_exec123_note_ram_write16(bus);
            return;
        }
#endif
        {
            const uint dst = m68ki_read_16(ea);
            const uint res = src + dst;
            const uint out = MASK_OUT_ABOVE_16(res);
            FLAG_N = NFLAG_16(res);
            FLAG_V = VFLAG_ADD_16(src, dst, res);
            FLAG_X = FLAG_C = CFLAG_16(res);
            FLAG_Z = out;
            m68ki_write_16(ea, out);
            return;
        }
    }
    if ((op & 0xf1f8u) == 0xe048u) { /* R57E129A: LSR.W #n,Dn */
        uint * const r_dst = &DY;
        const uint shift = (((op >> 9) - 1u) & 7u) + 1u; /* exactly 1..8 */
        const uint src = MASK_OUT_ABOVE_16(*r_dst);
        const uint res = src >> shift;
        USE_CYCLES(shift << CYC_SHIFT);
        *r_dst = MASK_OUT_BELOW_16(*r_dst) | res;
        FLAG_N = NFLAG_CLEAR;
        FLAG_Z = res;
        FLAG_X = FLAG_C = src << (9u - shift);
        FLAG_V = VFLAG_CLEAR;
        return;
    }
    if ((op & 0xf1f8u) == 0x1010u) { /* R57E130A: MOVE.B (An),Dn */
        const uint ea = AY;
        uint res;
#ifdef ESP_PLATFORM
        const uint32_t bus = (uint32_t)ADDRESS_68K(ea);
        if (__builtin_expect(bus < 0x00c00000u, 1)) {
            BusErrFlag = 0;
            res = (uint)MEM[bus ^ 1u];
        } else
#endif
            res = m68ki_read_8(ea);
        {
            uint * const r_dst = &DX;
            *r_dst = MASK_OUT_BELOW_8(*r_dst) | res;
        }
        FLAG_N = NFLAG_8(res);
        FLAG_Z = res;
        FLAG_V = VFLAG_CLEAR;
        FLAG_C = CFLAG_CLEAR;
        return;
    }
    if ((op & 0xf1f8u) == 0xd068u) { /* R57E130A: ADD.W d16(An),Dn */
        const uint ea = AY + MAKE_INT_16(m68ki_read_imm_16());
        uint src;
#ifdef ESP_PLATFORM
        const uint32_t bus = (uint32_t)ADDRESS_68K(ea);
        if (__builtin_expect(((ea & 1u) == 0u) && bus <= 0x00bffffeu, 1)) {
            BusErrFlag = 0;
            src = (uint)tab5_data584_native_word_load(MEM + bus);
        } else
#endif
            src = m68ki_read_16(ea);
        {
            uint * const r_dst = &DX;
            const uint dst = MASK_OUT_ABOVE_16(*r_dst);
            const uint res = src + dst;
            FLAG_N = NFLAG_16(res);
            FLAG_V = VFLAG_ADD_16(src, dst, res);
            FLAG_X = FLAG_C = CFLAG_16(res);
            FLAG_Z = MASK_OUT_ABOVE_16(res);
            *r_dst = MASK_OUT_BELOW_16(*r_dst) | FLAG_Z;
        }
        return;
    }
    if ((op & 0xfff8u) == 0x0c40u) { /* R57E130A: CMPI.W #imm,Dn */
        const uint src = m68ki_read_imm_16();
        const uint dst = MASK_OUT_ABOVE_16(DY);
        const uint res = dst - src;
        FLAG_N = NFLAG_16(res);
        FLAG_Z = MASK_OUT_ABOVE_16(res);
        FLAG_V = VFLAG_SUB_16(src, dst, res);
        FLAG_C = CFLAG_16(res);
        return;
    }
    if ((op & 0xfff8u) == 0x4a00u) { /* R57E130A: TST.B Dn */
        const uint res = MASK_OUT_ABOVE_8(DY);
        FLAG_N = NFLAG_8(res);
        FLAG_Z = res;
        FLAG_V = VFLAG_CLEAR;
        FLAG_C = CFLAG_CLEAR;
        return;
    }
    if ((op & 0xfff8u) == 0x4a50u) { /* TST.W (An) */
        const uint res = OPER_AY_AI_16();
        FLAG_N = NFLAG_16(res);
        FLAG_Z = res;
        FLAG_V = VFLAG_CLEAR;
        FLAG_C = CFLAG_CLEAR;
        return;
    }
    if ((op & 0xfff8u) == 0x4a40u) { /* TST.W Dn */
        const uint res = MASK_OUT_ABOVE_16(DY);
        FLAG_N = NFLAG_16(res);
        FLAG_Z = res;
        FLAG_V = VFLAG_CLEAR;
        FLAG_C = CFLAG_CLEAR;
        return;
    }
    if ((op & 0xfff8u) == 0x4840u) { /* SWAP Dn */
        uint * const r_dst = &DY;
        FLAG_Z = MASK_OUT_ABOVE_32(*r_dst << 16);
        *r_dst = (*r_dst >> 16) | FLAG_Z;
        FLAG_Z = *r_dst;
        FLAG_N = NFLAG_32(*r_dst);
        FLAG_C = CFLAG_CLEAR;
        FLAG_V = VFLAG_CLEAR;
        return;
    }
    if ((op & 0xf100u) == 0x7000u) { /* MOVEQ */
        const uint res = DX = MAKE_INT_8(MASK_OUT_ABOVE_8(op));
        FLAG_N = NFLAG_32(res);
        FLAG_Z = res;
        FLAG_V = VFLAG_CLEAR;
        FLAG_C = CFLAG_CLEAR;
        return;
    }
    if ((op & 0xf1f8u) == 0x1000u) { /* Build 5.98g7: MOVE.B Dn,Dm */
        const uint res = MASK_OUT_ABOVE_8(DY);
        uint * const r_dst = &DX;
        *r_dst = MASK_OUT_BELOW_8(*r_dst) | res;
        FLAG_N = NFLAG_8(res);
        FLAG_Z = res;
        FLAG_V = VFLAG_CLEAR;
        FLAG_C = CFLAG_CLEAR;
        return;
    }
    if ((op & 0xf1f8u) == 0x30c0u) { /* Build 5.98g7: MOVE.W Dn,(An)+ */
        const uint res = MASK_OUT_ABOVE_16(DY);
        const uint ea = EA_AX_PI_16();
        m68ki_write_16(ea, res);
        FLAG_N = NFLAG_16(res);
        FLAG_Z = res;
        FLAG_V = VFLAG_CLEAR;
        FLAG_C = CFLAG_CLEAR;
        return;
    }
    if ((op & 0xf1f8u) == 0x20c0u) { /* Build 5.98g9: MOVE.L Dn,(An)+ */
        const uint res = DY;
        const uint ea = EA_AX_PI_32();
        m68ki_write_32(ea, res);
        FLAG_N = NFLAG_32(res);
        FLAG_Z = res;
        FLAG_V = VFLAG_CLEAR;
        FLAG_C = CFLAG_CLEAR;
        return;
    }
    if ((op & 0xf1f8u) == 0x20d8u) { /* Build 5.98g10: MOVE.L (An)+,(Am)+ */
        const uint res = OPER_AY_PI_32();
        const uint ea = EA_AX_PI_32();
        m68ki_write_32(ea, res);
        FLAG_N = NFLAG_32(res);
        FLAG_Z = res;
        FLAG_V = VFLAG_CLEAR;
        FLAG_C = CFLAG_CLEAR;
        return;
    }
    if ((op & 0xfff8u) == 0x0800u) { /* Build 5.98g10: BTST #imm,Dn */
        FLAG_Z = DY & (1u << (OPER_I_8() & 0x1fu));
        return;
    }
    if ((op & 0xfff8u) == 0x4298u) { /* Build 5.98g10: CLR.L (An)+ */
        m68ki_write_32(EA_AY_PI_32(), 0);
        FLAG_N = NFLAG_CLEAR;
        FLAG_V = VFLAG_CLEAR;
        FLAG_C = CFLAG_CLEAR;
        FLAG_Z = ZFLAG_SET;
        return;
    }
    if ((op & 0xfff8u) == 0x48c0u) { /* Build 5.98g10: EXT.L Dn */
        uint * const r_dst = &DY;
        *r_dst = MASK_OUT_ABOVE_16(*r_dst) | (GET_MSB_16(*r_dst) ? 0xffff0000u : 0u);
        FLAG_N = NFLAG_32(*r_dst);
        FLAG_Z = *r_dst;
        FLAG_V = VFLAG_CLEAR;
        FLAG_C = CFLAG_CLEAR;
        return;
    }
    if ((op & 0xfff8u) == 0x4258u) { /* Build 5.98g9: CLR.W (An)+ */
        m68ki_write_16(EA_AY_PI_16(), 0);
        FLAG_N = NFLAG_CLEAR;
        FLAG_V = VFLAG_CLEAR;
        FLAG_C = CFLAG_CLEAR;
        FLAG_Z = ZFLAG_SET;
        return;
    }
    if ((op & 0xf1f8u) == 0x5048u || (op & 0xf1f8u) == 0x5088u) {
        /* Address-register ADDQ ignores the encoded .W/.L distinction for
         * arithmetic width and does not modify CCR; this mirrors Musashi. */
        uint * const r_dst = &AY;
        *r_dst = MASK_OUT_ABOVE_32(*r_dst + (((op >> 9) - 1u) & 7u) + 1u);
        return;
    }
    if ((op & 0xf1f8u) == 0x3000u) { /* MOVE.W Dn,Dm */
        const uint res = MASK_OUT_ABOVE_16(DY);
        uint * const r_dst = &DX;
        *r_dst = MASK_OUT_BELOW_16(*r_dst) | res;
        FLAG_N = NFLAG_16(res);
        FLAG_Z = res;
        FLAG_V = VFLAG_CLEAR;
        FLAG_C = CFLAG_CLEAR;
        return;
    }
    if ((op & 0xf1f8u) == 0x2000u) { /* MOVE.L Dn,Dm */
        const uint res = DY;
        DX = res;
        FLAG_N = NFLAG_32(res);
        FLAG_Z = res;
        FLAG_V = VFLAG_CLEAR;
        FLAG_C = CFLAG_CLEAR;
        return;
    }
    if ((op & 0xf1f8u) == 0xd040u) { /* ADD.W Dn,Dm */
        uint * const r_dst = &DX;
        const uint src = MASK_OUT_ABOVE_16(DY);
        const uint dst = MASK_OUT_ABOVE_16(*r_dst);
        const uint res = src + dst;
        FLAG_N = NFLAG_16(res);
        FLAG_V = VFLAG_ADD_16(src, dst, res);
        FLAG_X = FLAG_C = CFLAG_16(res);
        FLAG_Z = MASK_OUT_ABOVE_16(res);
        *r_dst = MASK_OUT_BELOW_16(*r_dst) | FLAG_Z;
        return;
    }
    if ((op & 0xf1f8u) == 0xc040u) { /* AND.W Dn,Dm */
        FLAG_Z = MASK_OUT_ABOVE_16(DX &= (DY | 0xffff0000u));
        FLAG_N = NFLAG_16(FLAG_Z);
        FLAG_C = CFLAG_CLEAR;
        FLAG_V = VFLAG_CLEAR;
        return;
    }
    if ((op & 0xf1ffu) == 0xc07cu) { /* AND.W #imm,Dn */
        FLAG_Z = MASK_OUT_ABOVE_16(DX &= (OPER_I_16() | 0xffff0000u));
        FLAG_N = NFLAG_16(FLAG_Z);
        FLAG_C = CFLAG_CLEAR;
        FLAG_V = VFLAG_CLEAR;
        return;
    }
    if ((op & 0xf1ffu) == 0xc0bcu) { /* AND.L #imm,Dn */
        FLAG_Z = DX &= OPER_I_32();
        FLAG_N = NFLAG_32(FLAG_Z);
        FLAG_C = CFLAG_CLEAR;
        FLAG_V = VFLAG_CLEAR;
        return;
    }
    if ((op & 0xfff8u) == 0x0240u) { /* ANDI.W #imm,Dn */
        FLAG_Z = MASK_OUT_ABOVE_16(DY &= (OPER_I_16() | 0xffff0000u));
        FLAG_N = NFLAG_16(FLAG_Z);
        FLAG_C = CFLAG_CLEAR;
        FLAG_V = VFLAG_CLEAR;
        return;
    }
    if ((op & 0xfff8u) == 0x0280u) { /* ANDI.L #imm,Dn */
        FLAG_Z = DY &= OPER_I_32();
        FLAG_N = NFLAG_32(FLAG_Z);
        FLAG_C = CFLAG_CLEAR;
        FLAG_V = VFLAG_CLEAR;
        return;
    }
    if ((op & 0xf1f8u) == 0x5040u) { /* ADDQ.W #n,Dn */
        uint * const r_dst = &DY;
        const uint src = (((op >> 9) - 1u) & 7u) + 1u;
        const uint dst = MASK_OUT_ABOVE_16(*r_dst);
        const uint res = src + dst;
        FLAG_N = NFLAG_16(res);
        FLAG_V = VFLAG_ADD_16(src, dst, res);
        FLAG_X = FLAG_C = CFLAG_16(res);
        FLAG_Z = MASK_OUT_ABOVE_16(res);
        *r_dst = MASK_OUT_BELOW_16(*r_dst) | FLAG_Z;
        return;
    }
    if ((op & 0xf1f8u) == 0x5140u) { /* SUBQ.W #n,Dn */
        uint * const r_dst = &DY;
        const uint src = (((op >> 9) - 1u) & 7u) + 1u;
        const uint dst = MASK_OUT_ABOVE_16(*r_dst);
        const uint res = dst - src;
        FLAG_N = NFLAG_16(res);
        FLAG_Z = MASK_OUT_ABOVE_16(res);
        FLAG_X = FLAG_C = CFLAG_16(res);
        FLAG_V = VFLAG_SUB_16(src, dst, res);
        *r_dst = MASK_OUT_BELOW_16(*r_dst) | FLAG_Z;
        return;
    }
    if ((op & 0xf1f8u) == 0xe040u || (op & 0xf1f8u) == 0xe080u) { /* ASR #n,Dn */
        uint * const r_dst = &DY;
        const uint shift = (((op >> 9) - 1u) & 7u) + 1u;
        if ((op & 0x00c0u) == 0x0080u) { /* long */
            const uint src = *r_dst;
            uint res = src >> shift;
            USE_CYCLES(shift << CYC_SHIFT);
            if (GET_MSB_32(src)) res |= m68ki_shift_32_table[shift];
            *r_dst = res;
            FLAG_N = NFLAG_32(res);
            FLAG_Z = res;
            FLAG_V = VFLAG_CLEAR;
            FLAG_X = FLAG_C = src << (9u - shift);
        } else { /* word */
            const uint src = MASK_OUT_ABOVE_16(*r_dst);
            uint res = src >> shift;
            USE_CYCLES(shift << CYC_SHIFT);
            if (GET_MSB_16(src)) res |= m68ki_shift_16_table[shift];
            *r_dst = MASK_OUT_BELOW_16(*r_dst) | res;
            FLAG_N = NFLAG_16(res);
            FLAG_Z = res;
            FLAG_V = VFLAG_CLEAR;
            FLAG_X = FLAG_C = src << (9u - shift);
        }
        return;
    }
}

typedef struct {
    uint32_t sig_cycles;      /* bit31 valid, bits30:24 post flags, bits23:16 cycles, bits15:0 opcode */
    tab5_m68k_handler_t handler;
} tab5_m68k_dispatch_entry_t;

/* ========================================================================
 * R57E139A5: X68P4 PREDECODE LOCALITY v2 + REGION-GATED COHERENCY
 * ------------------------------------------------------------------------
 * Runtime opcode decode is forbidden in the normal RAM/IPL instruction loop.
 * A 128-byte guest code page is decoded as one cold operation into Internal
 * SRAM before any instruction in that page is executed.  The hot path then
 * performs only:
 *
 *      PC -> direct page slot -> X68P4Op[(PC & 0x1ff) >> 1] -> execute
 *
 * No opcode load from guest RAM, no opcode-indexed L1/L2 lookup and no R123
 * fused lookup remain on a resident-page hit.  v0 deliberately keeps the
 * already-validated concrete Musashi semantic function pointer as a migration
 * bridge *inside the predecoded op*.  The page ABI is independent so R139+ can
 * replace `sem` with native X68P4 uops without changing page/cache/coherency.
 *
 * Code-page coherency is write driven.  Existing PX68K RAM writers already
 * call m68k_tab5_exec123_invalidate_range/all(); R138 redirects those public
 * invalidators to the X68P4 page directory. */
#define X68P4_PRE138_PAGE_SHIFT    7u
#define X68P4_PRE138_PAGE_BYTES    (1u << X68P4_PRE138_PAGE_SHIFT)
#define X68P4_PRE138_PAGE_MASK     (X68P4_PRE138_PAGE_BYTES - 1u)
#define X68P4_PRE138_WORDS_PER_PAGE (X68P4_PRE138_PAGE_BYTES / 2u)
#define X68P4_PRE138_CACHE_BYTES   (64u * 1024u)
#define X68P4_PRE138_OP_BYTES      8u
#define X68P4_PRE138_PAGE_OP_BYTES (X68P4_PRE138_WORDS_PER_PAGE * X68P4_PRE138_OP_BYTES)
#define X68P4_PRE138_PAGE_SLOTS    (X68P4_PRE138_CACHE_BYTES / X68P4_PRE138_PAGE_OP_BYTES)
#define X68P4_PRE138_WAYS          2u
#define X68P4_PRE138_SET_COUNT     (X68P4_PRE138_PAGE_SLOTS / X68P4_PRE138_WAYS)
#define X68P4_PRE138_SET_MASK      (X68P4_PRE138_SET_COUNT - 1u)
#define X68P4_PRE138_REGION_SHIFT  12u
#define X68P4_PRE138_REGION_COUNT  (0x00c00000u >> X68P4_PRE138_REGION_SHIFT)
#define X68P4_PRE138_INVALID_TAG   0xffffffffu

typedef struct {
    uint32_t sig_cycles; /* same compact payload as old fused entry, but page-AOT */
    tab5_m68k_handler_t sem;
} x68p4_pre138_op_t;

_Static_assert(sizeof(x68p4_pre138_op_t) == X68P4_PRE138_OP_BYTES,
               "X68P4Op v0 must stay 8 bytes on ESP32-P4");
_Static_assert((X68P4_PRE138_PAGE_SLOTS & (X68P4_PRE138_PAGE_SLOTS - 1u)) == 0u,
               "X68P4 page slots must be power-of-two");

static DRAM_ATTR x68p4_pre138_op_t *s_x68p4_pre138_ops = NULL;
static DRAM_ATTR uint32_t s_x68p4_pre138_cache_bytes = 0u;
/* R57E139A: exported tiny page-directory metadata is the single coherency
 * authority shared by CPU direct stores and HD63450 DMA writes.  The 64 KiB
 * decoded payload stays private; 128 tags/versions plus the compact 4 KiB
 * resident-region directory are visible to the inline write barriers. */
DRAM_ATTR uint32_t g_x68p4_pre139_page_tag[X68P4_PRE138_PAGE_SLOTS];
DRAM_ATTR uint32_t g_x68p4_pre139_page_version[X68P4_PRE138_PAGE_SLOTS];
DRAM_ATTR uint8_t g_x68p4_pre139_region_count[X68P4_PRE138_REGION_COUNT];
DRAM_ATTR uint32_t g_x68p4_pre139_enabled = 0u;
DRAM_ATTR uint32_t g_x68p4_pre139_epoch = 1u;
static DRAM_ATTR uint8_t s_x68p4_pre139_replace[X68P4_PRE138_SET_COUNT];

/* R140X5: cold-only opcode metadata L1.
 *
 * X4 measured demand-decode at 0.2%, 7.1% and 13.1% of CPU time as the
 * workload moved from resident code into churn-heavy code.  The pre-R138
 * dispatch L1 already proved that a 256-entry direct cache with this folded
 * opcode hash hits about 87% on the same SFXVI family of workloads.
 *
 * This cache is consulted ONLY when a PC word has not yet become an X68P4Op.
 * Resident X68P4Op execution never touches it, so the X3 hot path is unchanged.
 * Entries contain only immutable Musashi metadata: handler, base cycles and
 * post-op classification.  Guest code bytes and coherency remain page-owned. */
#define X68P4_X5_COLD_META_BITS 8u
#define X68P4_X5_COLD_META_SIZE (1u << X68P4_X5_COLD_META_BITS)
#define X68P4_X5_COLD_META_MASK (X68P4_X5_COLD_META_SIZE - 1u)
static DRAM_ATTR tab5_m68k_dispatch_entry_t
    s_x68p4_x5_cold_meta[X68P4_X5_COLD_META_SIZE];

static inline __attribute__((always_inline)) unsigned
x68p4_x5_cold_meta_index(uint16_t op)
{
    return (unsigned)(op ^ (op >> 7) ^ (op >> 12)) & X68P4_X5_COLD_META_MASK;
}

static inline __attribute__((always_inline)) unsigned
x68p4_pre138_set(uint32_t page_base)
{
    /* A5 keeps A4's XOR fold, but maps to 64 sets x 2 ways.  Resident-page
     * instructions still use the saved local slot generation and never pay
     * this hash. */
    uint32_t pn = page_base >> X68P4_PRE138_PAGE_SHIFT;
    pn ^= pn >> 7;
    pn ^= pn >> 14;
    return (unsigned)(pn & X68P4_PRE138_SET_MASK);
}

static inline __attribute__((always_inline)) void
x68p4_pre139_region_add(uint32_t page)
{
    if (page >= 0x00c00000u) return;
    const unsigned r = (unsigned)(page >> X68P4_PRE138_REGION_SHIFT);
    uint8_t v = g_x68p4_pre139_region_count[r];
    if (v != 0xffu) g_x68p4_pre139_region_count[r] = (uint8_t)(v + 1u);
}

static inline __attribute__((always_inline)) void
x68p4_pre139_region_remove(uint32_t page)
{
    if (page >= 0x00c00000u) return;
    const unsigned r = (unsigned)(page >> X68P4_PRE138_REGION_SHIFT);
    uint8_t v = g_x68p4_pre139_region_count[r];
    if (v) {
        --v;
        g_x68p4_pre139_region_count[r] = v;
    }
}

static inline __attribute__((always_inline)) int
x68p4_pre139_invalidate_page_internal(uint32_t page)
{
    if (page >= 0x00c00000u) return 0;
    const unsigned r = (unsigned)(page >> X68P4_PRE138_REGION_SHIFT);
    if (!g_x68p4_pre139_region_count[r]) return 0;
    const unsigned first = x68p4_pre138_set(page) << 1;
    for (unsigned way = 0; way < X68P4_PRE138_WAYS; ++way) {
        const unsigned slot = first + way;
        if (g_x68p4_pre139_page_tag[slot] == page) {
            g_x68p4_pre139_page_tag[slot] = X68P4_PRE138_INVALID_TAG;
            ++g_x68p4_pre139_page_version[slot];
            if (!g_x68p4_pre139_page_version[slot]) g_x68p4_pre139_page_version[slot] = 1u;
            m68k_tab5_x68p4_bump_epoch();
            x68p4_pre139_region_remove(page);
            return 1;
        }
    }
    return 0;
}

static inline __attribute__((always_inline)) x68p4_pre138_op_t *
x68p4_pre138_slot_ops(unsigned slot)
{
    return s_x68p4_pre138_ops + ((uint32_t)slot * X68P4_PRE138_WORDS_PER_PAGE);
}

static void x68p4_pre138_invalidate_all_internal(void)
{
    for (unsigned i = 0; i < X68P4_PRE138_PAGE_SLOTS; ++i) {
        g_x68p4_pre139_page_tag[i] = X68P4_PRE138_INVALID_TAG;
        ++g_x68p4_pre139_page_version[i];
        if (!g_x68p4_pre139_page_version[i]) g_x68p4_pre139_page_version[i] = 1u;
    }
    m68k_tab5_x68p4_bump_epoch();
    memset(g_x68p4_pre139_region_count, 0, sizeof(g_x68p4_pre139_region_count));
}

static void x68p4_pre138_invalidate_range_internal(uint32_t address, uint32_t bytes)
{
    if (!g_x68p4_pre139_enabled || !bytes) return;
    address &= 0x00ffffffu;
    if (address >= 0x00c00000u) return; /* IPL is immutable; devices are not cached. */
    if (bytes > 0x00c00000u - address) bytes = 0x00c00000u - address;
    if (bytes >= 8192u) {
        x68p4_pre138_invalidate_all_internal();
        return;
    }
    const uint32_t first = address & ~X68P4_PRE138_PAGE_MASK;
    const uint32_t last = (address + bytes - 1u) & ~X68P4_PRE138_PAGE_MASK;
    for (uint32_t p = first;; p += X68P4_PRE138_PAGE_BYTES) {
        (void)x68p4_pre139_invalidate_page_internal(p);
        if (p == last) break;
    }
}

void m68k_tab5_x68p4_predecode_bind(void *base, unsigned int bytes)
{
    g_x68p4_pre139_enabled = 0u;
    s_x68p4_pre138_ops = NULL;
    s_x68p4_pre138_cache_bytes = 0u;
    if (!base || bytes < X68P4_PRE138_CACHE_BYTES ||
        (((uintptr_t)base & 63u) != 0u)) {
        return;
    }
    s_x68p4_pre138_ops = (x68p4_pre138_op_t *)base;
    s_x68p4_pre138_cache_bytes = X68P4_PRE138_CACHE_BYTES;
    memset(s_x68p4_pre138_ops, 0, X68P4_PRE138_CACHE_BYTES);
    memset(g_x68p4_pre139_page_tag, 0xff, sizeof(g_x68p4_pre139_page_tag));
    memset(g_x68p4_pre139_page_version, 0, sizeof(g_x68p4_pre139_page_version));
    memset(g_x68p4_pre139_region_count, 0, sizeof(g_x68p4_pre139_region_count));
    memset(s_x68p4_pre139_replace, 0, sizeof(s_x68p4_pre139_replace));
    memset(s_x68p4_x5_cold_meta, 0, sizeof(s_x68p4_x5_cold_meta));
    g_x68p4_pre139_epoch = 1u;
    for (unsigned i = 0; i < X68P4_PRE138_PAGE_SLOTS; ++i) {
        g_x68p4_pre139_page_tag[i] = X68P4_PRE138_INVALID_TAG;
        g_x68p4_pre139_page_version[i] = 1u;
    }
    g_x68p4_pre139_enabled = 1u;
}

/* R140X5 demand-decode + cold metadata reuse.
 *
 * A4/A5 proved that page-fill amplification dominates when locality is poor:
 * the old fill path decoded all 64 even-word positions in every 128-byte page,
 * including extension words that were never executed as opcodes.  X2's return
 * to 512-byte pages made this substantially worse on the same guest workload.
 *
 * Keep the proven 128-byte / 2-way directory and exact R139 coherency, but on
 * page allocation clear only the 512-byte payload and decode the actual PC word
 * on first execution.  A non-NULL semantic pointer is the per-word valid bit.
 * Re-entering the same word is the same hot path plus one strongly-taken NULL
 * check; no new opcode semantics or cycle accounting are introduced.
 */
static __attribute__((noinline)) x68p4_pre138_op_t *
x68p4_pre138_decode_word(uint32_t page_base, unsigned slot, unsigned word_index)
{
    x68p4_pre138_op_t * const xop =
        x68p4_pre138_slot_ops(slot) + word_index;
    if (__builtin_expect(xop->sem != NULL, 1))
        return xop;

    const int ram = page_base <= (0x00c00000u - X68P4_PRE138_PAGE_BYTES);
    const int ipl = page_base >= 0x00fc0000u &&
                    page_base <= (0x01000000u - X68P4_PRE138_PAGE_BYTES);
    if (!ram && !ipl)
        return NULL;

    const uint8_t * const src = ram ? (MEM + page_base)
                                    : (IPL + (page_base & 0x0003ffffu));
    uint16_t op;
    __builtin_memcpy(&op, src + (word_index << 1), sizeof(op));

    /* R140X5 cold metadata reuse.  The guest opcode itself is still fetched
     * from the authoritative RAM/IPL page on first execution.  Only immutable
     * host metadata is reused across PCs carrying the same opcode. */
    const unsigned mi = x68p4_x5_cold_meta_index(op);
    tab5_m68k_dispatch_entry_t * const me = &s_x68p4_x5_cold_meta[mi];
    const uint32_t msig = me->sig_cycles;
    tab5_m68k_handler_t sem;
    uint32_t packed;
    if (__builtin_expect(me->handler != NULL &&
                         (uint16_t)(msig & 0xffffu) == op, 1)) {
        sem = me->handler;
        packed = msig;
    } else {
        sem = m68ki_instruction_jump_table[op];
        const uint32_t cyc = CYC_INSTRUCTION[op] & 0xffu;
        const uint32_t post = tab5_post585_flags(op) & 0x7fu;
        packed = (post << 24) | (cyc << 16) | (uint32_t)op;
        me->handler = sem;
        me->sig_cycles = packed;
    }

    xop->sig_cycles = 0x80000000u | packed;
    /* Publish semantic pointer after the compact metadata.  The valid bit in
     * sig_cycles is consumed only by CPU1's executor; page invalidation still
     * discards the entire X68P4Op generation exactly as in X3. */
    xop->sem = sem;
    return xop;
}

static __attribute__((noinline)) x68p4_pre138_op_t *
x68p4_pre138_fill_page(uint32_t pc, uint32_t *version_out, unsigned *slot_out)
{
    if (!g_x68p4_pre139_enabled || !s_x68p4_pre138_ops) return NULL;
    const uint32_t page_base = pc & ~X68P4_PRE138_PAGE_MASK;
    const int ram = page_base <= (0x00c00000u - X68P4_PRE138_PAGE_BYTES);
    const int ipl = page_base >= 0x00fc0000u &&
                    page_base <= (0x01000000u - X68P4_PRE138_PAGE_BYTES);
    if (!ram && !ipl) return NULL;

    const unsigned set = x68p4_pre138_set(page_base);
    const unsigned first = set << 1;
    unsigned slot;
    if (g_x68p4_pre139_page_tag[first] == X68P4_PRE138_INVALID_TAG)
        slot = first;
    else if (g_x68p4_pre139_page_tag[first + 1u] == X68P4_PRE138_INVALID_TAG)
        slot = first + 1u;
    else
        slot = first + (unsigned)(s_x68p4_pre139_replace[set] & 1u);
    s_x68p4_pre139_replace[set] = (uint8_t)((slot & 1u) ^ 1u);

    const uint32_t old_page = g_x68p4_pre139_page_tag[slot];
    if (old_page != X68P4_PRE138_INVALID_TAG)
        x68p4_pre139_region_remove(old_page);

    x68p4_pre138_op_t * const dst = x68p4_pre138_slot_ops(slot);
    /* Invalidate all old per-word semantic pointers in one compact internal
     * SRAM clear.  This replaces 64 opcode fetches + 64 table lookups +
     * 64 post-flag decodes on every page miss. */
    memset(dst, 0, X68P4_PRE138_PAGE_OP_BYTES);

    g_x68p4_pre139_page_tag[slot] = page_base;
    if (ram) x68p4_pre139_region_add(page_base);
    ++g_x68p4_pre139_page_version[slot];
    if (!g_x68p4_pre139_page_version[slot]) g_x68p4_pre139_page_version[slot] = 1u;
    if (version_out) *version_out = g_x68p4_pre139_page_version[slot];
    if (slot_out) *slot_out = slot;
    return dst;
}

static inline __attribute__((always_inline)) x68p4_pre138_op_t *
x68p4_pre138_resolve(uint32_t pc, uint32_t *local_tag, uint32_t *local_version,
                     unsigned *local_slot, x68p4_pre138_op_t **local_ops,
                     uint32_t *local_epoch)
{
    const uint32_t page_base = pc & ~X68P4_PRE138_PAGE_MASK;

    /* R140P1: the write side bumps one scalar only when a resident code page
     * is actually invalidated.  Same-page execution therefore avoids the
     * indexed page_version[slot] dependency/load on every instruction. */
    if (__builtin_expect(*local_ops != NULL && *local_tag == page_base &&
                         *local_epoch == g_x68p4_pre139_epoch, 1)) {
        return *local_ops + ((pc & X68P4_PRE138_PAGE_MASK) >> 1);
    }

    const unsigned set = x68p4_pre138_set(page_base);
    const unsigned first = set << 1;
    unsigned slot = first;
    x68p4_pre138_op_t *ops = NULL;
    uint32_t ver = 0u;
    if (g_x68p4_pre139_page_tag[first] == page_base) {
        slot = first;
        ops = x68p4_pre138_slot_ops(slot);
    } else if (g_x68p4_pre139_page_tag[first + 1u] == page_base) {
        slot = first + 1u;
        ops = x68p4_pre138_slot_ops(slot);
    }
    if (ops) {
        ver = g_x68p4_pre139_page_version[slot];
        s_x68p4_pre139_replace[set] = (uint8_t)((slot & 1u) ^ 1u);
    } else {
        ops = x68p4_pre138_fill_page(pc, &ver, &slot);
    }
    if (!ops) {
        *local_ops = NULL;
        *local_tag = X68P4_PRE138_INVALID_TAG;
        *local_version = 0u;
        *local_slot = 0u;
        *local_epoch = g_x68p4_pre139_epoch;
        return NULL;
    }
    *local_ops = ops;
    *local_tag = page_base;
    *local_version = ver;
    *local_slot = slot;
    *local_epoch = g_x68p4_pre139_epoch;
    return ops + ((pc & X68P4_PRE138_PAGE_MASK) >> 1);
}



/* ------------------------------------------------------------------------ */
/* R140S2 static compile-time superinstruction layer                        */
/*                                                                          */
/* R140S1 proved that generic warm CORE_FAST chaining is not profitable:    */
/* its first 4096 followers required 4090 chain calls (1.001 follower/run),  */
/* and frame209->392 regressed 5.61s -> 5.86s.  That mechanism is completely */
/* retired here.  R140S2 keeps only fixed, compile-time SFXVI kernels at     */
/* known PCs.  There is no runtime discovery, descriptor, hash, JIT or VM.   */
/* ------------------------------------------------------------------------ */
#ifdef ESP_PLATFORM

typedef struct {
    uint32_t version;
    uint8_t slot;
    uint8_t valid;
    uint8_t initialized;
} tab5_r140s2_cert760_t;

typedef struct {
    uint32_t version;
    uint8_t slot;
    uint8_t valid_mask;
    uint8_t initialized;
} tab5_r140s2_cert784_t;

static DRAM_ATTR tab5_r140s2_cert760_t s_r140s2_cert760;
static DRAM_ATTR tab5_r140s2_cert784_t s_r140s2_cert784;

static inline __attribute__((always_inline)) uint16_t
tab5_r140s2_code16(uint32_t a)
{
    return (uint16_t)tab5_data584_native_word_load(MEM + a);
}

static inline __attribute__((always_inline)) int
tab5_r140s2_sig760(void)
{
    return tab5_r140s2_code16(0x00368760u)==0x3819u &&
           tab5_r140s2_code16(0x00368762u)==0x2004u &&
           tab5_r140s2_code16(0x00368764u)==0xc0bcu &&
           tab5_r140s2_code16(0x0036876au)==0xe480u &&
           tab5_r140s2_code16(0x0036876cu)==0x3204u &&
           tab5_r140s2_code16(0x0036876eu)==0xc27cu &&
           tab5_r140s2_code16(0x00368772u)==0xe441u &&
           tab5_r140s2_code16(0x00368774u)==0x3404u &&
           tab5_r140s2_code16(0x00368776u)==0xc47cu &&
           tab5_r140s2_code16(0x0036877au)==0x3819u;
}

static inline __attribute__((always_inline)) int
tab5_r140s2_sig784_at(uint32_t p)
{
    return tab5_r140s2_code16(p+0u)==0xe483u &&
           tab5_r140s2_code16(p+2u)==0xd043u &&
           tab5_r140s2_code16(p+4u)==0x3604u &&
           tab5_r140s2_code16(p+6u)==0xc67cu &&
           tab5_r140s2_code16(p+10u)==0xe443u &&
           tab5_r140s2_code16(p+12u)==0xd243u &&
           tab5_r140s2_code16(p+14u)==0x3604u &&
           tab5_r140s2_code16(p+16u)==0xc67cu &&
           tab5_r140s2_code16(p+20u)==0xd443u &&
           tab5_r140s2_code16(p+22u)==0x381au &&
           tab5_r140s2_code16(p+24u)==0x2604u &&
           tab5_r140s2_code16(p+26u)==0xc6bcu;
}

static inline __attribute__((always_inline)) int
tab5_r140s2_cert760_ok(uint32_t page_tag, uint32_t page_version, unsigned page_slot)
{
    if (__builtin_expect(page_tag != 0x00368700u, 0)) return 0;
    if (!s_r140s2_cert760.initialized ||
        s_r140s2_cert760.version != page_version ||
        s_r140s2_cert760.slot != (uint8_t)page_slot) {
        s_r140s2_cert760.version = page_version;
        s_r140s2_cert760.slot = (uint8_t)page_slot;
        s_r140s2_cert760.valid = (uint8_t)tab5_r140s2_sig760();
        s_r140s2_cert760.initialized = 1u;
    }
    return s_r140s2_cert760.valid != 0u;
}

static inline __attribute__((always_inline)) unsigned
tab5_r140s2_cert784_mask(uint32_t page_tag, uint32_t page_version, unsigned page_slot)
{
    if (__builtin_expect(page_tag != 0x00368780u, 0)) return 0u;
    if (!s_r140s2_cert784.initialized ||
        s_r140s2_cert784.version != page_version ||
        s_r140s2_cert784.slot != (uint8_t)page_slot) {
        unsigned mask = 0u;
        if (tab5_r140s2_sig784_at(0x00368784u)) mask |= 1u;
        if (tab5_r140s2_sig784_at(0x003687a4u)) mask |= 2u;
        if (tab5_r140s2_sig784_at(0x003687c4u)) mask |= 4u;
        s_r140s2_cert784.version = page_version;
        s_r140s2_cert784.slot = (uint8_t)page_slot;
        s_r140s2_cert784.valid_mask = (uint8_t)mask;
        s_r140s2_cert784.initialized = 1u;
    }
    return (unsigned)s_r140s2_cert784.valid_mask;
}

static inline __attribute__((always_inline)) uint32_t
tab5_r140s2_imm32(uint32_t a)
{
    return ((uint32_t)tab5_r140s2_code16(a) << 16) | (uint32_t)tab5_r140s2_code16(a+2u);
}

static inline __attribute__((always_inline)) int
tab5_r140s2_ram_word(uint32_t areg, uint32_t *out)
{
    const uint32_t bus = ADDRESS_68K(areg);
    if ((areg & 1u) || bus > 0x00bffffeu) return 0;
    *out = (uint32_t)tab5_data584_native_word_load(MEM + bus);
    BusErrFlag = 0;
    return 1;
}

/* Static SFXVI prefix: 10 instructions / 28 bytes / 94 exact 68000 cycles.
 * Old native-fragment measurements independently established 48-cycle and
 * 28-cycle safe prefixes; choose only a prefix that fully fits this J2 slice. */
static __attribute__((noinline, hot, optimize("O3"))) int
tab5_r140s2_sfx760_try(uint32_t fetch_pc, uint32_t page_tag, uint32_t page_version, unsigned page_slot)
{
    if (fetch_pc != 0x00368760u || GET_CYCLES() < 28) return 0;
    if (!tab5_r140s2_cert760_ok(page_tag, page_version, page_slot)) return 0;

    uint32_t a1 = REG_A[1];
    uint32_t w0, w1 = 0u;
    if (!tab5_r140s2_ram_word(a1, &w0)) return 0;
    int tier = 1;
    if (GET_CYCLES() >= 48) tier = 2;
    if (GET_CYCLES() >= 94 && tab5_r140s2_ram_word(a1 + 2u, &w1)) tier = 3;

    uint32_t d4 = (REG_D[4] & 0xffff0000u) | (w0 & 0xffffu);
    uint32_t d0 = d4 & tab5_r140s2_imm32(0x00368766u);
    REG_D[4] = d4;
    REG_D[0] = d0;
    REG_A[1] = a1 + 2u;

    if (tier == 1) {
        FLAG_N = NFLAG_32(d0); FLAG_Z = d0; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
        REG_PPC = 0x00368764u; REG_IR = 0xc0bcu; REG_PC = 0x0036876au;
        USE_CYCLES(28);
        return 1;
    }

    {
        const uint32_t src = d0;
        d0 = src >> 2;
        if (GET_MSB_32(src)) d0 |= m68ki_shift_32_table[2];
        REG_D[0] = d0;
        d4 = REG_D[4];
        REG_D[1] = (REG_D[1] & 0xffff0000u) | (d4 & 0xffffu);
        FLAG_X = src << 7;
    }
    if (tier == 2) {
        const uint32_t z = REG_D[1] & 0xffffu;
        FLAG_N = NFLAG_16(z); FLAG_Z = z; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
        REG_PPC = 0x0036876cu; REG_IR = 0x3204u; REG_PC = 0x0036876eu;
        USE_CYCLES(48);
        return 1;
    }

    {
        uint32_t d1 = REG_D[1];
        d1 = (d1 & 0xffff0000u) | ((d1 & 0xffffu) & (tab5_r140s2_code16(0x00368770u) & 0xffffu));
        const uint32_t src1 = d1 & 0xffffu;
        uint32_t r1 = src1 >> 2;
        if (GET_MSB_16(src1)) r1 |= m68ki_shift_16_table[2];
        REG_D[1] = (d1 & 0xffff0000u) | (r1 & 0xffffu);
        FLAG_X = src1 << 7;

        uint32_t d2 = (REG_D[2] & 0xffff0000u) | (REG_D[4] & 0xffffu);
        d2 = (d2 & 0xffff0000u) | ((d2 & 0xffffu) & (tab5_r140s2_code16(0x00368778u) & 0xffffu));
        REG_D[2] = d2;

        REG_D[4] = (REG_D[4] & 0xffff0000u) | (w1 & 0xffffu);
        REG_A[1] = a1 + 4u;
        FLAG_N = NFLAG_16(w1); FLAG_Z = w1 & 0xffffu; FLAG_V = VFLAG_CLEAR; FLAG_C = CFLAG_CLEAR;
        REG_PPC = 0x0036877au; REG_IR = 0x3819u; REG_PC = 0x0036877cu;
        USE_CYCLES(94);
        return 1;
    }
}

static inline __attribute__((always_inline)) unsigned
tab5_r140s2_pattern_bit(uint32_t pc)
{
    return pc == 0x00368784u ? 1u : pc == 0x003687a4u ? 2u : pc == 0x003687c4u ? 4u : 0u;
}

/* Repeating 12-op arithmetic body at $368784/$3687A4/$3687C4.  The three
 * tiers end after 5, 9 or 12 instructions, so J2 never moves a scheduler
 * boundary merely to enter the superinstruction. */
static __attribute__((noinline, hot, optimize("O3"))) int
tab5_r140s2_sfx784_try(uint32_t fetch_pc, uint32_t page_tag, uint32_t page_version, unsigned page_slot)
{
    const unsigned bit = tab5_r140s2_pattern_bit(fetch_pc);
    if (!bit) return 0;
    if (!(tab5_r140s2_cert784_mask(page_tag, page_version, page_slot) & bit)) return 0;

    const unsigned need5 = (unsigned)CYC_INSTRUCTION[0xe483u] + (unsigned)CYC_INSTRUCTION[0xd043u] +
                           (unsigned)CYC_INSTRUCTION[0x3604u] + (unsigned)CYC_INSTRUCTION[0xc67cu] +
                           (unsigned)CYC_INSTRUCTION[0xe443u] + 8u;
    const unsigned need9 = need5 + (unsigned)CYC_INSTRUCTION[0xd243u] +
                           (unsigned)CYC_INSTRUCTION[0x3604u] + (unsigned)CYC_INSTRUCTION[0xc67cu] +
                           (unsigned)CYC_INSTRUCTION[0xd443u];
    const unsigned need12 = need9 + (unsigned)CYC_INSTRUCTION[0x381au] +
                            (unsigned)CYC_INSTRUCTION[0x2604u] + (unsigned)CYC_INSTRUCTION[0xc6bcu];
    if ((unsigned)GET_CYCLES() < need5) return 0;

    unsigned tier = 5u;
    uint32_t loaded = 0u;
    uint32_t a2 = REG_A[2];
    if ((unsigned)GET_CYCLES() >= need9) tier = 9u;
    if ((unsigned)GET_CYCLES() >= need12 && tab5_r140s2_ram_word(a2, &loaded)) tier = 12u;

    uint32_t d3 = REG_D[3];
    {
        const uint32_t src = d3;
        d3 = src >> 2;
        if (GET_MSB_32(src)) d3 |= m68ki_shift_32_table[2];
    }
    {
        const uint32_t src = d3 & 0xffffu, dst = REG_D[0] & 0xffffu;
        const uint32_t res = src + dst;
        REG_D[0] = (REG_D[0] & 0xffff0000u) | (res & 0xffffu);
    }
    d3 = (d3 & 0xffff0000u) | (REG_D[4] & 0xffffu);
    d3 = (d3 & 0xffff0000u) | ((d3 & 0xffffu) & tab5_r140s2_code16(fetch_pc + 8u));
    {
        const uint32_t src = d3 & 0xffffu; uint32_t r = src >> 2;
        if (GET_MSB_16(src)) r |= m68ki_shift_16_table[2];
        d3 = (d3 & 0xffff0000u) | (r & 0xffffu);
        FLAG_X = FLAG_C = src << 7; FLAG_N = NFLAG_16(r); FLAG_Z = r & 0xffffu; FLAG_V = VFLAG_CLEAR;
    }
    REG_D[3] = d3;
    if (tier == 5u) {
        REG_PPC = fetch_pc + 10u; REG_IR = 0xe443u; REG_PC = fetch_pc + 12u;
        USE_CYCLES(need5);
        return 1;
    }

    {
        const uint32_t src = d3 & 0xffffu, dst = REG_D[1] & 0xffffu;
        const uint32_t res = src + dst;
        REG_D[1] = (REG_D[1] & 0xffff0000u) | (res & 0xffffu);
    }
    d3 = (d3 & 0xffff0000u) | (REG_D[4] & 0xffffu);
    d3 = (d3 & 0xffff0000u) | ((d3 & 0xffffu) & tab5_r140s2_code16(fetch_pc + 18u));
    {
        const uint32_t src = d3 & 0xffffu, dst = REG_D[2] & 0xffffu;
        const uint32_t res = src + dst;
        REG_D[2] = (REG_D[2] & 0xffff0000u) | (res & 0xffffu);
        FLAG_N = NFLAG_16(res); FLAG_Z = res & 0xffffu;
        FLAG_X = FLAG_C = CFLAG_16(res); FLAG_V = VFLAG_ADD_16(src, dst, res);
    }
    REG_D[3] = d3;
    if (tier == 9u) {
        REG_PPC = fetch_pc + 20u; REG_IR = 0xd443u; REG_PC = fetch_pc + 22u;
        USE_CYCLES(need9);
        return 1;
    }

    REG_D[4] = (REG_D[4] & 0xffff0000u) | (loaded & 0xffffu);
    REG_A[2] = a2 + 2u;
    d3 = REG_D[4] & tab5_r140s2_imm32(fetch_pc + 28u);
    REG_D[3] = d3;
    FLAG_N = NFLAG_32(d3); FLAG_Z = d3; FLAG_C = CFLAG_CLEAR; FLAG_V = VFLAG_CLEAR;
    /* X remains from the preceding ADD.W, exactly as MOVE/AND preserve it. */
    REG_PPC = fetch_pc + 26u; REG_IR = 0xc6bcu; REG_PC = fetch_pc + 32u;
    USE_CYCLES(need12);
    return 1;
}


/* ------------------------------------------------------------------------ */
/* R140M2 MDX exact-PC compile-time fused block certification               */
/* ------------------------------------------------------------------------ */
/* M1B measured these exact residual paths. Unlike BAT160 broad memory-op
 * classification and BAT161 BE01 shell handling, M2 certifies only fixed
 * same-128B-page basic blocks. Page-version changes force revalidation.
 * No runtime trace/descriptor/discovery is introduced. */
#define R140M2_BLOCKS 10u
static const uint16_t s_r140m2_sig_62a[20] = {
    0xb228u, 0x0002u, 0x6708u, 0x08c6u, 0x000cu, 0x1141u, 0x0002u, 0x0800u,
    0x0006u, 0x56c1u, 0xb228u, 0x0003u, 0x6708u, 0x08c6u, 0x000du, 0x1141u,
    0x0003u, 0x7000u, 0x102bu, 0x0018u,
};
static const uint16_t s_r140m2_sig_666[11] = {
    0x102bu, 0x001fu, 0xb028u, 0x0005u, 0x6708u, 0x08c6u, 0x0000u, 0x1140u,
    0x0005u, 0x102bu, 0x001eu,
};
static const uint16_t s_r140m2_sig_68a[11] = {
    0x302bu, 0x0010u, 0xb068u, 0x0008u, 0x6708u, 0x08c6u, 0x0002u, 0x3140u,
    0x0008u, 0x302bu, 0x0036u,
};
static const uint16_t s_r140m2_sig_6c4[5] = {
    0x4880u, 0x4440u, 0xb068u, 0x000eu, 0x6708u,
};
static const uint16_t s_r140m2_sig_6f2[6] = {
    0x7000u, 0x102bu, 0x0022u, 0x0880u, 0x0007u, 0x6610u,
};
static const uint16_t s_r140m2_sig_708[9] = {
    0x1031u, 0x0000u, 0x6006u, 0x4400u, 0xd03cu, 0x007fu, 0xb028u, 0x0012u,
    0x6708u,
};
static const uint16_t s_r140m2_sig_730[24] = {
    0x6708u, 0x08c6u, 0x0008u, 0x1140u, 0x0013u, 0x2013u, 0xb0a8u, 0x0014u,
    0x6708u, 0x08c6u, 0x0009u, 0x2140u, 0x0014u, 0x302bu, 0x0012u, 0xb068u,
    0x0018u, 0x6708u, 0x08c6u, 0x000au, 0x3140u, 0x0018u, 0x302bu, 0x0014u,
};
static const uint16_t s_r140m2_sig_7f0[6] = {
    0x7a00u, 0x2685u, 0x3029u, 0x0014u, 0x9069u, 0x0012u,
};
static const uint16_t s_r140m2_sig_800[11] = {
    0xb06bu, 0x0008u, 0x6708u, 0x3740u, 0x0008u, 0x08c5u, 0x0001u, 0x70c0u,
    0xc029u, 0x001cu, 0xe518u,
};
static const uint16_t s_r140m2_sig_86c[10] = {
    0xb02bu, 0x0010u, 0x670au, 0x177cu, 0x0001u, 0x0003u, 0x1740u, 0x0010u,
    0x1029u, 0x0023u,
};

typedef struct {
    uint32_t version;
    uint8_t slot;
    uint16_t valid_mask;
    uint8_t initialized;
} tab5_r140m2_pagecert_t;

static DRAM_ATTR tab5_r140m2_pagecert_t s_r140m2_cert_600;
static DRAM_ATTR tab5_r140m2_pagecert_t s_r140m2_cert_680;
static DRAM_ATTR tab5_r140m2_pagecert_t s_r140m2_cert_700;
static DRAM_ATTR tab5_r140m2_pagecert_t s_r140m2_cert_780;
static DRAM_ATTR tab5_r140m2_pagecert_t s_r140m2_cert_800;

/* Perfect 6-bit hash for the ten exact entry PCs. */
static DRAM_ATTR uint32_t s_r140m2_gate[64] = {
    [0]  = 0x0019d800u,
    [4]  = 0x0019d708u,
    [5]  = 0x0019d68au,
    [21] = 0x0019d62au,
    [24] = 0x0019d730u,
    [34] = 0x0019d6c4u,
    [51] = 0x0019d666u,
    [54] = 0x0019d86cu,
    [56] = 0x0019d7f0u,
    [57] = 0x0019d6f2u,
};

static inline __attribute__((always_inline)) int
tab5_r140m2_sig_words(uint32_t start, const uint16_t *sig, unsigned count)
{
    unsigned i;
    for (i = 0; i < count; ++i)
        if (tab5_r140s2_code16(start + i * 2u) != sig[i]) return 0;
    return 1;
}

static inline __attribute__((always_inline)) unsigned tab5_r140m2_refresh_600(void)
{
    unsigned m = 0u;
    if (tab5_r140m2_sig_words(0x0019d62au, s_r140m2_sig_62a, sizeof(s_r140m2_sig_62a)/sizeof(s_r140m2_sig_62a[0]))) m |= 1u<<0;
    if (tab5_r140m2_sig_words(0x0019d666u, s_r140m2_sig_666, sizeof(s_r140m2_sig_666)/sizeof(s_r140m2_sig_666[0]))) m |= 1u<<1;
    return m;
}
static inline __attribute__((always_inline)) unsigned tab5_r140m2_refresh_680(void)
{
    unsigned m = 0u;
    if (tab5_r140m2_sig_words(0x0019d68au, s_r140m2_sig_68a, sizeof(s_r140m2_sig_68a)/sizeof(s_r140m2_sig_68a[0]))) m |= 1u<<2;
    if (tab5_r140m2_sig_words(0x0019d6c4u, s_r140m2_sig_6c4, sizeof(s_r140m2_sig_6c4)/sizeof(s_r140m2_sig_6c4[0]))) m |= 1u<<3;
    if (tab5_r140m2_sig_words(0x0019d6f2u, s_r140m2_sig_6f2, sizeof(s_r140m2_sig_6f2)/sizeof(s_r140m2_sig_6f2[0]))) m |= 1u<<4;
    return m;
}
static inline __attribute__((always_inline)) unsigned tab5_r140m2_refresh_700(void)
{
    unsigned m = 0u;
    if (tab5_r140m2_sig_words(0x0019d708u, s_r140m2_sig_708, sizeof(s_r140m2_sig_708)/sizeof(s_r140m2_sig_708[0]))) m |= 1u<<5;
    if (tab5_r140m2_sig_words(0x0019d730u, s_r140m2_sig_730, sizeof(s_r140m2_sig_730)/sizeof(s_r140m2_sig_730[0]))) m |= 1u<<6;
    return m;
}
static inline __attribute__((always_inline)) unsigned tab5_r140m2_refresh_780(void)
{
    return tab5_r140m2_sig_words(0x0019d7f0u, s_r140m2_sig_7f0, sizeof(s_r140m2_sig_7f0)/sizeof(s_r140m2_sig_7f0[0])) ? (1u<<7) : 0u;
}
static inline __attribute__((always_inline)) unsigned tab5_r140m2_refresh_800(void)
{
    unsigned m = 0u;
    if (tab5_r140m2_sig_words(0x0019d800u, s_r140m2_sig_800, sizeof(s_r140m2_sig_800)/sizeof(s_r140m2_sig_800[0]))) m |= 1u<<8;
    if (tab5_r140m2_sig_words(0x0019d86cu, s_r140m2_sig_86c, sizeof(s_r140m2_sig_86c)/sizeof(s_r140m2_sig_86c[0]))) m |= 1u<<9;
    return m;
}

static inline __attribute__((always_inline)) int tab5_r140m2_gate_pc(uint32_t pc)
{
    if (__builtin_expect((pc & 0x00fff000u) != 0x0019d000u, 1)) return 0;
    return s_r140m2_gate[(pc >> 1) & 63u] == pc;
}

/* R140P4S5C1: exact first opcodes for the ten already-certified M2 entries.
 * These are not a new decode/discovery table: every value is the first word
 * of the compile-time signature immediately above.  The early path is used
 * only while the same 128-byte X68P4 page is already resident and its scalar
 * coherency epoch still matches; otherwise the authoritative resolver below
 * runs unchanged. */
static inline __attribute__((always_inline)) uint16_t
tab5_r140p4s5c1_m2_first_op(uint32_t pc)
{
    switch (pc) {
    case 0x0019d62au: return 0xb228u;
    case 0x0019d666u: return 0x102bu;
    case 0x0019d68au: return 0x302bu;
    case 0x0019d6c4u: return 0x4880u;
    case 0x0019d6f2u: return 0x7000u;
    case 0x0019d708u: return 0x1031u;
    case 0x0019d730u: return 0x6708u;
    case 0x0019d7f0u: return 0x7a00u;
    case 0x0019d800u: return 0xb06bu;
    case 0x0019d86cu: return 0xb02bu;
    default: return 0u;
    }
}

static inline __attribute__((always_inline)) int
tab5_r140m2_cert_ok(uint32_t pc, uint32_t page_tag, uint32_t page_version, unsigned page_slot)
{
    tab5_r140m2_pagecert_t *c;
    unsigned bit, mask;
    switch (page_tag) {
    case 0x0019d600u: c=&s_r140m2_cert_600; bit=(pc==0x0019d62au)?(1u<<0):(1u<<1); break;
    case 0x0019d680u: c=&s_r140m2_cert_680; bit=(pc==0x0019d68au)?(1u<<2):(pc==0x0019d6c4u)?(1u<<3):(1u<<4); break;
    case 0x0019d700u: c=&s_r140m2_cert_700; bit=(pc==0x0019d708u)?(1u<<5):(1u<<6); break;
    case 0x0019d780u: c=&s_r140m2_cert_780; bit=1u<<7; break;
    case 0x0019d800u: c=&s_r140m2_cert_800; bit=(pc==0x0019d800u)?(1u<<8):(1u<<9); break;
    default: return 0;
    }
    if (!c->initialized || c->version != page_version || c->slot != (uint8_t)page_slot) {
        switch (page_tag) {
        case 0x0019d600u: mask=tab5_r140m2_refresh_600(); break;
        case 0x0019d680u: mask=tab5_r140m2_refresh_680(); break;
        case 0x0019d700u: mask=tab5_r140m2_refresh_700(); break;
        case 0x0019d780u: mask=tab5_r140m2_refresh_780(); break;
        case 0x0019d800u: mask=tab5_r140m2_refresh_800(); break;
        default: mask=0u; break;
        }
        c->version=page_version; c->slot=(uint8_t)page_slot;
        c->valid_mask=(uint16_t)mask; c->initialized=1u;
    }
    return (c->valid_mask & bit) != 0u;
}

#endif


/* ------------------------------------------------------------------------ */
/* R140J2 X68P4 deadline-driven Machine Kernel                              */
/* ------------------------------------------------------------------------ */
extern uint32_t VLINE;
extern int ICount;
extern int ClkUsed;

#define X68P4_J2_DMA_SAFE_PERIPH 200


static DRAM_ATTR int s_x68p4_machine_key_int_cnt = 0;
static DRAM_ATTR int s_x68p4_machine_mouse_int_cnt = 0;

/* Convert a peripheral-clock deadline into the smallest positive guest-CPU
 * request whose existing ClkUsed conversion reaches that deadline.  This is
 * the inverse of the exact conversion already used after m68k_execute(). */
static inline __attribute__((always_inline)) int
x68p4_j2_cpu_until_periph_p12(int periph)
{
    if (periph <= 0) return 1;
    /* X68K Tab P12C1 product clock is fixed at 12 MHz.  This is the exact
     * inverse of the selected 10/12 peripheral conversion. */
    int64_t need = (int64_t)periph * 12LL - (int64_t)ClkUsed;
    if (need <= 0) return 1;
    int64_t cpu = (need + 9) / 10;
    if (cpu < 1) cpu = 1;
    if (cpu > 0x3fffffffLL) cpu = 0x3fffffffLL;
    return (int)cpu;
}

static inline __attribute__((always_inline)) int
x68p4_j2_choose_request_p12(int remaining)
{
    if (__builtin_expect(remaining <= 1, 0))
        return remaining;

    /* R140P4MK1 COMMON0: a deadline only matters if its converted CPU request
     * is strictly smaller than 'remaining'.  Compute the first peripheral
     * deadline that cannot shorten this slice and use it as a strict sentinel.
     * RC1 measured reason=0 on 145146/145641 slices, so the normal path now
     * returns without the old 64-bit inverse conversion. */
    const unsigned early_periph =
        ((unsigned)ClkUsed + (unsigned)(remaining - 1) * 10u) / 12u;
    const int no_early_deadline = (int)early_periph + 1;
    int best_periph = no_early_deadline;

    /* MFP's exact next IRQ deadline is maintained as a shadow at guest state
     * changes / true crossings.  The common choose path is one load+compare. */
    int d = MFP_R140MK1DeadlineMin(best_periph);
    if (d < best_periph) best_periph = d;

    /* RTC can only preempt when MFP IRQ15 is enabled, unmasked and not already
     * in service.  That gate is cached with the same MFP interrupt state. */
    if (__builtin_expect(MFP_R140MK1IRQ15Open != 0u, 0)) {
        d = RTC_R140J2NextIRQDeadline(best_periph);
        if (d < best_periph) best_periph = d;
    }

    /* 68450 readiness is callback/device driven rather than represented by a
     * guest-clock counter.  Keep the proven 200@10MHz service deadline only
     * while channels 0..2 are actually active. */
    if (__builtin_expect(((DMA[0].CSR | DMA[1].CSR | DMA[2].CSR) & 0x08u) != 0u, 0) &&
        X68P4_J2_DMA_SAFE_PERIPH < best_periph) {
        best_periph = X68P4_J2_DMA_SAFE_PERIPH;
    }

    /* Timer/interrupt register polling keeps the exact J2 200-clock guard. */
    if (__builtin_expect(MFP_R140J2PollGuard != 0u, 0) &&
        X68P4_J2_DMA_SAFE_PERIPH < best_periph) {
        best_periph = X68P4_J2_DMA_SAFE_PERIPH;
    }

    if (__builtin_expect(best_periph == no_early_deadline, 1))
        return remaining;

    const int deadline_cpu = x68p4_j2_cpu_until_periph_p12(best_periph);
    return (deadline_cpu < remaining) ? deadline_cpu : remaining;
}

int m68k_tab5_x68p4_machine_run_scanline(
    uint32_t line, uint32_t total_lines, int cpu_cycles,
    unsigned int midi_delay, int *periph_cycles_out)
{
    int clk_line = 0;
    int total_executed = 0;
    MFP_Int(0);
    if ((line >= CRTC_VSTART) && (line < CRTC_VEND))
        VLINE = ((line - CRTC_VSTART) * CRTC_VStep) / 2u;
    else
        VLINE = (uint32_t)-1;

    if (!(MFP[MFP_AER] & 0x40u) && (line == CRTC_IntLine))
        MFP_Int(1);
    if (MFP[MFP_AER] & 0x10u) {
        if (line == CRTC_VSTART) MFP_Int(9);
    } else {
        if (CRTC_VEND >= total_lines) {
            if ((long)line == (long)(CRTC_VEND - total_lines)) MFP_Int(9);
        } else if ((long)line == (long)(total_lines - 1u)) {
            MFP_Int(9);
        }
    }

    int remaining = cpu_cycles;
    while (remaining > 0) {
        const int request = x68p4_j2_choose_request_p12(remaining);
        int executed = m68k_execute(request);
        if (executed <= 0) executed = request;

        total_executed += executed;
        remaining -= executed;
        if (remaining < 0) remaining = 0;
        ICount -= executed;
        if (ICount < 0) ICount = 0;

        int usedclk;
        if (__builtin_expect((unsigned)ClkUsed < 12u, 1)) {
            const unsigned acc = (unsigned)ClkUsed + (unsigned)executed * 10u;
            usedclk = (int)(acc / 12u);
            ClkUsed = (int)(acc - (unsigned)usedclk * 12u);
        } else {
            /* Defensive save-state/foreign-context fallback only.  Normal P12
             * execution maintains ClkUsed in the 0..11 numerator domain. */
            ClkUsed += executed * 10;
            usedclk = ClkUsed / 12;
            ClkUsed -= usedclk * 12;
        }
        clk_line += usedclk;

        MFP_Timer(usedclk);
        RTC_Timer(usedclk);
        DMA_ExecActive012Inline();
    }

    if (__builtin_expect(MFP_R140J2PollGuard != 0u, 0))
        --MFP_R140J2PollGuard;

    /* Timer-A event-count mode remains frozen at the exact scanline/vline
     * boundary that is already known to be boot-sensitive. */
    if (__builtin_expect(MIDI_R127DelayPending != 0u, 0))
        MIDI_DelayOut(midi_delay);
    MFP_TimerA();
    if ((MFP[MFP_AER] & 0x40u) && (line == CRTC_IntLine))
        MFP_Int(1);

    *periph_cycles_out = clk_line;
    return total_executed;
}

void m68k_tab5_x68p4_machine_finish_scanline(int periph_cycles,
                                              int key_int_period,
                                              int mouse_int_period)
{

    ADPCM_R127_PreCounter += (int)(ADPCM_R127_PreStepCurrent * (uint32_t)periph_cycles);
    if (__builtin_expect(ADPCM_R127_PreCounter >= 10000000L, 0))
        ADPCM_PreUpdateR127Due();
    OPM_Timer((uint32_t)periph_cycles);
    if (__builtin_expect(MIDI_R127TimerActive != 0u, 0))
        MIDI_Timer((uint32_t)periph_cycles);

    if (++s_x68p4_machine_key_int_cnt > key_int_period) {
        s_x68p4_machine_key_int_cnt = 0;
        Keyboard_Int();
    }
    if (++s_x68p4_machine_mouse_int_cnt > mouse_int_period) {
        s_x68p4_machine_mouse_int_cnt = 0;
        SCC_IntCheck();
    }
}

void m68k_tab5_x68p4_machine_frame_end(void)
{
    FDD_SetFDInt();
}

/* ABI-compatible zero-stat stubs: production has no runtime accounting. */
void m68k_tab5_x68p4_machine_stats(uint64_t *lines, uint64_t *slices,
                                    uint64_t *periph_cycles)
{
    if (lines) *lines = 0u;
    if (slices) *slices = 0u;
    if (periph_cycles) *periph_cycles = 0u;
}

void m68k_tab5_x68p4_predecode_stats(uint32_t *fill, uint32_t *hit, uint32_t *miss,
                                     uint32_t *inv, uint32_t *escape)
{
    if (fill) *fill = 0u;
    if (hit) *hit = 0u;
    if (miss) *miss = 0u;
    if (inv) *inv = 0u;
    if (escape) *escape = 0u;
}

#ifdef TCM_DRAM_ATTR
static TCM_DRAM_ATTR tab5_m68k_dispatch_entry_t s_tab5_dispatch_cache[TAB5_M68K_DISPATCH_CACHE_SIZE];
#else
static DRAM_ATTR tab5_m68k_dispatch_entry_t s_tab5_dispatch_cache[TAB5_M68K_DISPATCH_CACHE_SIZE];
#endif

static DRAM_ATTR tab5_m68k_dispatch_entry_t s_tab5_dispatch_l2[TAB5_M68K_DISPATCH_L2_SIZE];

/* R57E123A: 1024-entry persistent fused execution cache in Internal RAM.
 * A hit avoids both the guest-RAM opcode load and opcode-indexed L1/L2 dispatch
 * lookup.  Entry epochs are killed by RAM writes; the global epoch changes only
 * for explicit full barriers, not at every m68k_execute() slice. */
DRAM_ATTR tab5_exec123_entry_t g_tab5_exec123_cache[TAB5_EXEC123_SIZE];
DRAM_ATTR uint32_t g_tab5_exec123_epoch = 1u;

void m68k_tab5_exec123_invalidate_all(void)
{
#if PX68K_TAB5_X68P4_PREDECODE
    x68p4_pre138_invalidate_all_internal();
    /* Keep the legacy epoch moving only for ABI/debug readers. */
    if (++g_tab5_exec123_epoch == 0u) g_tab5_exec123_epoch = 1u;
#else
    if (++g_tab5_exec123_epoch == 0u) {
        memset(g_tab5_exec123_cache, 0, sizeof(g_tab5_exec123_cache));
        g_tab5_exec123_epoch = 1u;
    }
#endif
}

void m68k_tab5_exec123_invalidate_range(uint32_t address, uint32_t bytes)
{
#if PX68K_TAB5_X68P4_PREDECODE
    x68p4_pre138_invalidate_range_internal(address, bytes);
#else
    address &= 0x00ffffffu;
    if (!bytes || address >= 0x00c00000u) return;
    if (bytes > 0x00c00000u - address) bytes = 0x00c00000u - address;
    if (bytes >= 2048u) {
        m68k_tab5_exec123_invalidate_all();
        return;
    }
    uint32_t a = address & ~1u;
    const uint32_t end = (address + bytes + 1u) & ~1u;
    for (; a < end; a += 2u)
        g_tab5_exec123_cache[tab5_exec123_index(a)].epoch = 0u;
#endif
}


uint32_t m68k_tab5_dispatch_tcm_bytes(void)
{
    return (uint32_t)sizeof(s_tab5_dispatch_cache);
}

uint32_t m68k_tab5_dispatch_l2_bytes(void)
{
    return (uint32_t)sizeof(s_tab5_dispatch_l2);
}

/* R139A6 production-clean: historical poll/DBF/stream/fill/fast-family
 * attribution counters physically removed.  Only the functional P4 stream
 * backend enable state remains. */
static DRAM_ATTR int s_tab5_stream581_pie_enabled = 0;

extern int px68k_m68k_zero_fill_ram(uint32_t address, uint32_t bytes);
extern int px68k_m68k_repeat_fill_ram(uint32_t address, uint32_t value,
                                      uint32_t unit_bytes, uint32_t count);
extern uint32_t px68k_m68k_live_repeat_movew_ram(uint32_t code_address,
                                                  uint32_t dest_address,
                                                  uint32_t value,
                                                  uint32_t opcode,
                                                  uint32_t max_words);
extern uint32_t GVRAM_WriteWordRepeat256(uint32_t adr, uint16_t data, uint32_t count);
extern uint32_t GVRAM_WriteWordCopy256(uint32_t adr, const uint8_t *src_native_words,
                                       uint32_t count, uint16_t *last_word);
extern uint32_t GVRAM_CopyWordStream256(uint32_t src_adr, uint32_t dst_adr,
                                        uint32_t count, uint16_t *last_word);
extern int GVRAM_WriteWordRepeat256P4Eligible(uint32_t adr, uint32_t count);
extern int GVRAM_WriteWordCopy256P4Eligible(uint32_t adr, const uint8_t *src_native_words,
                                             uint32_t count);
extern uint32_t GVRAM_WriteWordRepeat256P4(uint32_t adr, uint16_t data, uint32_t count,
                                           uint32_t *pie_words);
extern uint32_t GVRAM_WriteWordCopy256P4(uint32_t adr, const uint8_t *src_native_words,
                                         uint32_t count, uint16_t *last_word,
                                         uint32_t *pie_words);
extern int GVRAM_P4StreamSelfcheck(void);
static inline __attribute__((always_inline)) uint16_t tab5_poll58_fetch16(uint32_t a);


/* Build 5.98g9: exact ordinary-RAM helpers for the hot MOVEM.L stack
 * prologue/epilogue pair. PX68K RAM is word-swapped on little-endian hosts;
 * two native 16-bit transfers reproduce m68k_read/write_memory_32 exactly. */
static inline __attribute__((always_inline)) uint32_t tab5_ram598g9_load32(uint32_t a)
{
    uint16_t hi, lo;
    __builtin_memcpy(&hi, MEM + a, 2u);
    __builtin_memcpy(&lo, MEM + a + 2u, 2u);
    BusErrFlag = 0;
    return ((uint32_t)hi << 16) | (uint32_t)lo;
}

static inline __attribute__((always_inline)) void tab5_ram598g9_store32(uint32_t a, uint32_t v)
{
    const uint16_t hi = (uint16_t)(v >> 16);
    const uint16_t lo = (uint16_t)v;
    __builtin_memcpy(MEM + a, &hi, 2u);
    __builtin_memcpy(MEM + a + 2u, &lo, 2u);
    BusErrFlag = 0;
    m68k_tab5_exec123_note_ram_write32(a);
}

/* Return non-zero only when the instruction was fully executed here. Odd or
 * non-RAM ranges fall back to the generated Musashi handler. */
static inline __attribute__((always_inline)) int tab5_movem598g9_try(uint16_t op)
{
    const uint32_t extpc = REG_PC & 0x00ffffffu;
    uint16_t mask;
    uint32_t count, bytes, ea, start, i;
    unsigned areg;

    if (FLAG_T1 || FLAG_T0) return 0;
    if (!((extpc <= 0x00bffffeu) ||
          (extpc >= 0x00fc0000u && extpc <= 0x00fffffeu))) return 0;

    mask = tab5_poll58_fetch16(extpc);
    count = (uint32_t)__builtin_popcount((unsigned)mask);
    bytes = count << 2;
    areg = op & 7u;
    ea = REG_A[areg];
    if (ea & 0xff000001u) return 0;

    if ((op & 0xfff8u) == 0x48e0u) { /* MOVEM.L regs,-(An) */
        if (bytes > ea) return 0;
        start = ea - bytes;
        if (ea > 0x00c00000u || (count && start > 0x00bffffcu)) return 0;
        (void)OPER_I_16();
        for (i = 0; i < 16u; ++i) {
            if (mask & (1u << i)) {
                ea -= 4u;
                tab5_ram598g9_store32(ea, REG_DA[15u - i]);
            }
        }
        REG_A[areg] = ea;
        USE_CYCLES((int)(count << CYC_MOVEM_L));
    } else if ((op & 0xfff8u) == 0x4cd8u) { /* MOVEM.L (An)+,regs */
        if (bytes > (0x00c00000u - ea)) return 0;
        if (count && ea > 0x00bffffcu) return 0;
        (void)OPER_I_16();
        for (i = 0; i < 16u; ++i) {
            if (mask & (1u << i)) {
                REG_DA[i] = tab5_ram598g9_load32(ea);
                ea += 4u;
            }
        }
        REG_A[areg] = ea;
        USE_CYCLES((int)(count << CYC_MOVEM_L));
    } else {
        return 0;
    }

    return 1;
}

/* Build 5.70: live, verified MOVE.W Dn,(An)+ run batching.
 *
 * 5.68/5.69 proved that the hot $34C4 sequences are large unrolled streams,
 * not necessarily loops that revisit an already learned PC range.  Therefore
 * learning after execution can never help the first (and often only) pass.
 *
 * The 5.70 path validates the actual RAM code at REG_PPC immediately before
 * batching.  The RAM helper scans forward only up to what the CURRENT Musashi
 * timeslice can execute, verifies every opcode is identical, rejects any
 * destination/code overlap, and performs only ordinary-RAM stores.
 *
 * Musashi in this PX68K tree checks external interrupts only at m68k_execute()
 * entry ("removed per-instruction interrupt checks"), so batching inside the
 * same timeslice does not remove an interrupt observation point. Trace mode is
 * rejected explicitly. Any odd/MMIO/non-RAM destination falls back to the
 * authoritative generated handler, preserving address/bus exception behavior.
 */
#define TAB5_MOVEW570_MAX_WORDS 128u


static inline __attribute__((always_inline))
int tab5_movew570_try_live_run(uint16_t op, uint32_t move_cycles)
{
    uint32_t pc, a, value, max_n, n, bytes;
    unsigned src_d, dst_a;
    int remain;

    if ((op & 0xf1f8u) != 0x30c0u || move_cycles == 0u || FLAG_T1 || FLAG_T0)
        return 0;

    remain = GET_CYCLES();
    if (remain <= (int)move_cycles)
        return 0;

    /* Normal Musashi executes the current instruction whenever cycles were
     * positive at loop entry. ceil(remain/cycles) therefore gives the maximum
     * homogeneous instructions that can be consumed without crossing the same
     * scheduler return point. */
    max_n = ((uint32_t)remain + move_cycles - 1u) / move_cycles;
    if (max_n > TAB5_MOVEW570_MAX_WORDS) max_n = TAB5_MOVEW570_MAX_WORDS;
    if (max_n < 2u) return 0;

    pc = REG_PPC & 0x00ffffffu;
    if ((pc & 1u) || pc > 0x00bffffeu)
        return 0;

    src_d = op & 7u;
    dst_a = (op >> 9) & 7u;
    a = REG_A[dst_a] & 0x00ffffffu;
    value = REG_D[src_d] & 0xffffu;

    /* Do not pre-mask away an invalid architectural address: the ordinary
     * Musashi path must see odd/high/MMIO destinations and raise/route them
     * exactly as before. */
    if (REG_A[dst_a] != a || (a & 1u) || a > 0x00bffffeu)
        return 0;

    n = px68k_m68k_live_repeat_movew_ram(pc, a, value, op, max_n);
    if (n < 2u)
    {
        return 0;
    }

    bytes = n << 1;
    REG_A[dst_a] = a + bytes;
    REG_PC = pc + bytes;

    /* All batched instructions move the same Dn word, so the final CCR equals
     * executing them individually. MOVE leaves X untouched. */
    FLAG_N = NFLAG_16(value);
    FLAG_Z = value;
    FLAG_V = VFLAG_CLEAR;
    FLAG_C = CFLAG_CLEAR;

    USE_CYCLES((int)(n * move_cycles));

    /* The current opcode has already gone through the normal fetch/profile/
     * dispatch-cache lookup. Account for the n-1 elided logical instructions
     * so sparse diagnostics continue to describe guest execution, not host
     * implementation details. */

    return 1;
}

/* Build 5.79: one generalized post-run stream engine rather than separate
 * opcode-specific accelerators.  Each call happens after one authoritative
 * Musashi instruction has already completed.  A descriptor records the
 * source class, destination class, width, registers and opcode; the common
 * engine owns opcode-prefix verification, scheduler-bound chunk sizing,
 * architectural register/PC/CCR updates and telemetry.
 *
 * Supported descriptor classes today:
 *   MOVE.W Dn,(Ad)+       -> 256-color GVRAM (constant stream)
 *   MOVE.W (As)+,(Ad)+    -> 256-color GVRAM (runtime RAM/GVRAM source backend)
 *
 * Adding MOVE.L or RAM destinations later is intentionally a descriptor /
 * backend extension, not another main execute-loop special case.
 */
#define TAB5_STREAM576_MAX_CHUNK 32u

typedef enum {
    TAB5_STREAM576_SRC_DREG = 0,
    TAB5_STREAM576_SRC_AREG_POSTINC = 1
} tab5_stream576_src_t;

typedef enum {
    TAB5_STREAM576_DST_GVRAM_POSTINC = 0
} tab5_stream576_dst_t;

typedef struct {
    uint16_t op;
    uint8_t width;
    uint8_t src_kind;
    uint8_t dst_kind;
    uint8_t src_reg;
    uint8_t dst_reg;
} tab5_stream576_desc_t;

static inline __attribute__((always_inline))
int tab5_stream576_decode(uint16_t op, tab5_stream576_desc_t *d)
{
    const uint16_t cls = op & 0xf1f8u;

    if (cls == 0x30c0u) {
        d->src_kind = TAB5_STREAM576_SRC_DREG;
    } else if (cls == 0x30d8u) {
        d->src_kind = TAB5_STREAM576_SRC_AREG_POSTINC;
    } else {
        return 0;
    }

    d->op = op;
    d->width = 2u;
    d->dst_kind = TAB5_STREAM576_DST_GVRAM_POSTINC;
    d->src_reg = (uint8_t)(op & 7u);
    d->dst_reg = (uint8_t)((op >> 9) & 7u);

    /* Source and destination postincrementing the same address register has
     * source-before-destination EA ordering that the simple two-pointer
     * backend intentionally does not emulate. */
    if (d->src_kind == TAB5_STREAM576_SRC_AREG_POSTINC && d->src_reg == d->dst_reg)
        return 0;
    return 1;
}


static inline __attribute__((always_inline))
uint32_t tab5_stream581_repeat_backend(uint32_t dst, uint16_t value, uint32_t n)
{
    uint32_t done, vec_words = 0;

    if (s_tab5_stream581_pie_enabled &&
        GVRAM_WriteWordRepeat256P4Eligible(dst, n))
    {
        done = GVRAM_WriteWordRepeat256P4(dst, value, n, &vec_words);
        if (__builtin_expect(done != 0u, 1))
        {
            return done;
        }
    }

    return GVRAM_WriteWordRepeat256(dst, value, n);
}

static inline __attribute__((always_inline))
uint32_t tab5_stream581_copy_backend(uint32_t dst, const uint8_t *src,
                                     uint32_t n, uint16_t *last_word)
{
    /* 5.80a live A/B: PIE copy was consistently ~1.5-1.7x the scalar
     * cycles/word. Keep the proven scalar path unconditionally. */
    return GVRAM_WriteWordCopy256(dst, src, n, last_word);
}

static inline __attribute__((always_inline))
int tab5_stream576_is_candidate(uint16_t op)
{
    const uint16_t cls = op & 0xf1f8u;
    return cls == 0x30c0u || cls == 0x30d8u;
}

static inline __attribute__((always_inline))
int tab5_stream576_try_postrun(uint16_t op, uint32_t move_cycles)
{
    tab5_stream576_desc_t d;
    uint32_t pc, max_n, n, dst_arch, dst, done, last_value;
    int remain;

    if (move_cycles == 0u || FLAG_T1 || FLAG_T0) return 0;
    if (!tab5_stream576_decode(op, &d)) return 0;

    remain = GET_CYCLES();
    if (remain <= 0) return 0;

    /* Normal Musashi starts another instruction whenever cycles are positive,
     * even if that final instruction takes the counter below zero. */
    max_n = ((uint32_t)remain + move_cycles - 1u) / move_cycles;
    if (max_n > TAB5_STREAM576_MAX_CHUNK) max_n = TAB5_STREAM576_MAX_CHUNK;
    if (max_n < 2u) return 0;

    pc = REG_PC & 0x00ffffffu;
    if ((pc & 1u) || pc > 0x00bffffeu) return 0;
    {
        const uint32_t code_words = (0x00c00000u - pc) >> 1;
        if (max_n > code_words) max_n = code_words;
        if (max_n < 2u) return 0;
    }

    /* One cheap lookahead gates the prefix scan. */
    if (tab5_poll58_fetch16(pc) != op) return 0;

    n = 1u;
    while (n < max_n && tab5_poll58_fetch16(pc + (n << 1)) == op)
        ++n;
    if (n < 2u) return 0;

    /* Build 5.78: A registers are architecturally 32 bit, but X68000 memory
     * cycles use a 24-bit bus.  Preserve the full A-register value for the
     * postincrement while decoding only the low 24 bits for the actual bus
     * access.  5.76/5.77 incorrectly rejected such aliases before reaching
     * the otherwise-safe RAM/GVRAM backends. */
    dst_arch = REG_A[d.dst_reg];
    dst = dst_arch & 0x00ffffffu;
    if ((dst & 1u) || dst < 0x00c00000u || dst >= 0x00d00000u) {
        return 0;
    }
    if (dst_arch & 0xff000000u)
    {
        const uint32_t dst_words = (0x00d00000u - dst) >> 1;
        if (n > dst_words) n = dst_words;
        if (n < 2u) {
            return 0;
        }
    }

    if (d.src_kind == TAB5_STREAM576_SRC_DREG) {
        last_value = REG_D[d.src_reg] & 0xffffu;
        done = tab5_stream581_repeat_backend(dst, (uint16_t)last_value, n);
        if (done != n) {
            return 0;
        }
    } else {
        const uint32_t src_arch = REG_A[d.src_reg];
        uint32_t src = src_arch & 0x00ffffffu;
        const int alias24 = (src_arch & 0xff000000u) != 0u;
        uint16_t last_word = 0;

        if (src & 1u) {
            return 0;
        }

        if (src < 0x00c00000u) {
            const uint32_t src_words = (0x00c00000u - src) >> 1;
            if (n > src_words) n = src_words;
            if (n < 2u) {
                return 0;
            }

            done = tab5_stream581_copy_backend(dst, MEM + src, n, &last_word);
            if (done != n) {
                return 0;
            }
        } else if (src < 0x00e00000u) {
            const uint32_t src_words = (0x00e00000u - src) >> 1;
            if (n > src_words) n = src_words;
            if (n < 2u) {
                return 0;
            }

            done = GVRAM_CopyWordStream256(src, dst, n, &last_word);
            if (done != n) {
                return 0;
            }
        } else if (src < 0x00e80000u) {
            /* Build 5.79: TVRAM reads are side-effect free.  TVRAM_Read() masks
             * the 512 KiB text-VRAM window and xor-swaps byte addresses on
             * little-endian hosts, which means an aligned guest word is stored
             * in host-native uint16_t order exactly like MEM/IPL.  Reuse the
             * common native-word -> GVRAM backend while keeping architectural
             * A-register postincrement and the scheduler boundary in the common
             * stream executor. */
            const uint32_t src_words = (0x00e80000u - src) >> 1;
            if (n > src_words) n = src_words;
            if (n < 2u) {
                return 0;
            }
            done = tab5_stream581_copy_backend(dst, TVRAM + (src & 0x0007ffffu), n, &last_word);
            if (done != n) {
                return 0;
            }
        } else if (src >= 0x00fc0000u) {
            /* IPL is a plain 256 KiB ROM in the same host-native word-swapped
             * representation as MEM.  Reads are side-effect free, so this is
             * a safe stream backend too. */
            const uint32_t src_words = (0x01000000u - src) >> 1;
            if (n > src_words) n = src_words;
            if (n < 2u) {
                return 0;
            }
            done = tab5_stream581_copy_backend(dst, IPL + (src & 0x0003ffffu), n, &last_word);
            if (done != n) {
                return 0;
            }
        } else {
            return 0;
        }

        if (alias24) {
        }

        BusErrFlag = 0;
        last_value = last_word;
        REG_A[d.src_reg] = src_arch + (n << 1);
    }

    REG_A[d.dst_reg] = dst_arch + (n << 1);
    REG_PC = pc + (n << 1);
    REG_PPC = pc + ((n - 1u) << 1);

    /* MOVE.W sets N/Z from the final transferred word, clears V/C and leaves
     * X untouched.  For memory-copy streams last_value is the final source. */
    FLAG_N = NFLAG_16(last_value);
    FLAG_Z = last_value;
    FLAG_V = VFLAG_CLEAR;
    FLAG_C = CFLAG_CLEAR;
    USE_CYCLES((int)(n * move_cycles));

    return 1;
}

static inline __attribute__((always_inline)) unsigned tab5_dispatch_index(uint16_t op)
{
    /* Fold both opcode bytes so register/mode variants do not all fight the
     * same low-byte slot.  Power-of-two indexing keeps the hit path tiny. */
    return (unsigned)(op ^ (op >> 7) ^ (op >> 12)) & TAB5_M68K_DISPATCH_CACHE_MASK;
}

static inline __attribute__((always_inline)) unsigned tab5_dispatch_l2_index(uint16_t op)
{
    return (unsigned)(op ^ (op >> 5) ^ (op >> 10) ^ (op >> 14)) & TAB5_M68K_DISPATCH_L2_MASK;
}

static void tab5_dispatch_cache_reset(void)
{
    memset(s_tab5_dispatch_cache, 0, sizeof(s_tab5_dispatch_cache));
    memset(s_tab5_dispatch_l2, 0, sizeof(s_tab5_dispatch_l2));
}

void m68k_tab5_dispatch_profile_set(int enabled) { (void)enabled; }

/*
 * Build 5.73 (ESP32-P4): scheduler-aware poll-loop accelerator.
 *
 * The PX68K scheduler advances ICount/MFP/RTC/DMA only after m68k_execute()
 * returns.  Therefore a taken read-only polling back-edge whose source is
 * ordinary RAM cannot observe a different value again inside the same CPU
 * slice: the loop itself writes no RAM and devices/DMA have not run yet.
 *
 * Recognized exact idioms are validated dynamically from guest opcodes:
 *   MOVE.B abs.l,Dn / AND.B #imm,Dn / BNE.B loop
 *   BTST   #imm,abs.l / BNE.B loop
 *   CMPI.W #imm,abs.l / BCS.B loop
 *
 * Ordinary RAM targets are fast-forwarded only to the current scheduler
 * boundary.  Existing known-safe MFP GPIP and FDC status targets retain the
 * same treatment.  MMIO/volatile/unknown regions always use normal Musashi.
 */
static inline __attribute__((always_inline)) uint16_t tab5_poll58_fetch16(uint32_t a)
{
    return (uint16_t)m68k_read_immediate_16(a & 0x00ffffffu);
}

static inline __attribute__((always_inline)) void tab5_poll59_fastforward(uint32_t kind_bit,
                                                                          uint32_t loop_pc)
{
    const int remain = GET_CYCLES();
    (void)kind_bit;
    (void)loop_pc;
    if (remain <= 0) return;




    /* The loop has already completed one normal taken iteration.  Ordinary
     * RAM cannot be changed by these read-only loop bodies before the PX68K
     * scheduler regains control.  Consume only the remainder of this CPU
     * slice; MFP/RTC/DMA/IRQ observation boundaries remain unchanged. */
    SET_CYCLES(0);
}

static inline __attribute__((always_inline)) void tab5_poll58_try(uint16_t branch_op)
{
    const uint32_t branch_pc = REG_PPC & 0x00ffffffu;
    const uint32_t loop_pc = REG_PC & 0x00ffffffu;
    uint16_t op0;
    uint32_t addr;

    if (GET_CYCLES() <= 0 || loop_pc >= branch_pc) return; /* not a taken back-edge */

    if (branch_op == 0x66f4u) {
        uint16_t op1;
        if (branch_pc != ((loop_pc + 10u) & 0x00ffffffu)) return;
        op0 = tab5_poll58_fetch16(loop_pc);
        op1 = tab5_poll58_fetch16(loop_pc + 6u);
        if ((op0 & 0xf1ffu) != 0x1039u) return; /* MOVE.B abs.l,Dn */
        if ((op1 & 0xf1ffu) != 0xc03cu) return; /* AND.B #imm,Dn */
        if ((op0 & 0x0e00u) != (op1 & 0x0e00u)) return; /* same Dn */
        if (tab5_poll58_fetch16(loop_pc + 10u) != branch_op) return;
        addr = ((uint32_t)tab5_poll58_fetch16(loop_pc + 2u) << 16)
             | (uint32_t)tab5_poll58_fetch16(loop_pc + 4u);
        addr &= 0x00ffffffu;
        if (addr == 0x00e88001u) {
            tab5_poll59_fastforward(1u, loop_pc);
            return;
        }
        if (addr == 0x00e94001u) {
            /* uPD72065 status read is side-effect free.  In this standalone
             * scheduler DMA ch0 advances only after m68k_execute() returns. */
            tab5_poll59_fastforward(4u, loop_pc);
            return;
        }
        if (addr < 0x00c00000u) {
            tab5_poll59_fastforward(8u, loop_pc);
            return;
        }
        return;
    }

    if (branch_op == 0x66f6u) {
        if (branch_pc != ((loop_pc + 8u) & 0x00ffffffu)) return;
        op0 = tab5_poll58_fetch16(loop_pc);
        if (op0 != 0x0839u) return; /* BTST #imm,abs.l */
        if (tab5_poll58_fetch16(loop_pc + 8u) != branch_op) return;
        addr = ((uint32_t)tab5_poll58_fetch16(loop_pc + 4u) << 16)
             | (uint32_t)tab5_poll58_fetch16(loop_pc + 6u);
        addr &= 0x00ffffffu;
        if (addr == 0x00e88001u) {
            tab5_poll59_fastforward(2u, loop_pc);
            return;
        }
        if (addr < 0x00c00000u) {
            tab5_poll59_fastforward(16u, loop_pc);
            return;
        }
        return;
    }

    /* Build 5.98f: exact two-instruction stable-RAM wait:
     *
     *     TST.W (An)
     *     BNE.B -4    (or BEQ.B -4)
     *
     * One complete taken iteration has already executed.  The body is
     * read-only and ordinary RAM cannot change until PX68K regains control,
     * so consume only the remainder of this m68k_execute() slice. */
    if (branch_op == 0x66fcu || branch_op == 0x67fcu) {
        unsigned areg;
        if (FLAG_T1 || FLAG_T0) return;
        if (branch_pc != ((loop_pc + 2u) & 0x00ffffffu)) return;
        op0 = tab5_poll58_fetch16(loop_pc);
        if ((op0 & 0xfff8u) != 0x4a50u) return;
        if (tab5_poll58_fetch16(loop_pc + 2u) != branch_op) return;
        areg = op0 & 7u;
        addr = REG_A[areg] & 0x00ffffffu;
        if (!(addr & 1u) && addr <= 0x00bffffeu) {
            tab5_poll59_fastforward(64u, loop_pc);
            return;
        }
        return;
    }

    /* Build 5.98g6: exact absolute-memory stable-RAM wait observed in SFXVI:
     *
     *     TST.W abs.l
     *     BNE.B -8    (or BEQ.B -8)
     *
     * The absolute operand makes this six-byte TST + two-byte branch.  As
     * with the proven (An) form above, ordinary RAM cannot change until the
     * PX68K scheduler regains control, so only the current CPU slice is
     * consumed.  MMIO/GVRAM/TVRAM remain authoritative and are never folded. */
    if (branch_op == 0x66f8u || branch_op == 0x67f8u) {
        if (FLAG_T1 || FLAG_T0) return;
        if (branch_pc != ((loop_pc + 6u) & 0x00ffffffu)) return;
        op0 = tab5_poll58_fetch16(loop_pc);
        if (op0 != 0x4a79u) return; /* TST.W abs.l */
        if (tab5_poll58_fetch16(loop_pc + 6u) != branch_op) return;
        addr = (((uint32_t)tab5_poll58_fetch16(loop_pc + 2u) << 16)
              | (uint32_t)tab5_poll58_fetch16(loop_pc + 4u)) & 0x00ffffffu;
        if (!(addr & 1u) && addr <= 0x00bffffeu) {
            tab5_poll59_fastforward(128u, loop_pc);
            return;
        }
        return;
    }

    /* CMPI.W #imm,abs.l / BCS.B back-edge.  Build 5.73 accelerates
     * only ordinary RAM sources; volatile/MMIO regions remain normal. */
    if (branch_op == 0x65f6u) {
        if (branch_pc != ((loop_pc + 8u) & 0x00ffffffu)) return;
        op0 = tab5_poll58_fetch16(loop_pc);
        if (op0 != 0x0c79u) return; /* CMPI.W #imm,abs.l */
        if (tab5_poll58_fetch16(loop_pc + 8u) != branch_op) return;
        addr = (((uint32_t)tab5_poll58_fetch16(loop_pc + 4u) << 16)
              | (uint32_t)tab5_poll58_fetch16(loop_pc + 6u)) & 0x00ffffffu;
        if (!(addr & 1u) && addr <= 0x00bffffeu) {
            tab5_poll59_fastforward(32u, loop_pc);
            return;
        }
    }
}

/* Build 6.15h17R17: exact scheduler-bounded fast-forward for the tiny
 * unsigned threshold-search loop measured inside the dominant MDX block:
 *
 *     loop: CMP.W (An)+,Dn
 *           BHI.B loop          ; exact displacement -4 / opcode $62FC
 *
 * This helper is called only after one complete architectural iteration and
 * taken BHI have already executed.  Future taken iterations are safe to fold
 * only while the postincrement source remains ordinary, aligned X68000 RAM.
 * The first not-taken comparison is deliberately left to normal Musashi, so
 * exit PC/flags and all surrounding code retain the authoritative path.
 *
 * CMP leaves X unchanged.  At a scheduler boundary N/Z/V/C must however match
 * the final elided CMP exactly, so those flags are reconstructed from the last
 * folded subtraction.  REG_PC already points at loop_pc after the taken BHI and
 * remains there after any number of additional taken iterations. */

static inline __attribute__((always_inline))
void tab5_cmphi617_try(uint16_t branch_op)
{
    const uint32_t branch_pc = REG_PPC & 0x00ffffffu;
    const uint32_t loop_pc = REG_PC & 0x00ffffffu;
    uint16_t cmp_op;
    unsigned areg, dreg;
    uint32_t addr, dst, n = 0u, max_n, last_res = 0u, last_src = 0u;
    int remain, loop_cycles;

    if (FLAG_T1 || FLAG_T0) return;
    if (branch_op != 0x62fcu) return;                /* BHI.B -4 only */
    if (branch_pc != ((loop_pc + 2u) & 0x00ffffffu)) return;

    cmp_op = tab5_poll58_fetch16(loop_pc);
    if ((cmp_op & 0xf1f8u) != 0xb058u) return;       /* CMP.W (An)+,Dn */
    if (tab5_poll58_fetch16(loop_pc + 2u) != branch_op) return;

    areg = cmp_op & 7u;
    dreg = (cmp_op >> 9) & 7u;
    addr = REG_A[areg];
    dst = REG_D[dreg] & 0xffffu;

    /* Direct RAM folding is allowed only for the exact non-aliased ordinary
     * RAM address space.  Odd, wrapped, MMIO, TVRAM/GVRAM and high aliases all
     * stay on stock Musashi. */
    if ((addr & 0xff000001u) || addr > 0x00bffffeu) return;

    loop_cycles = (int)CYC_INSTRUCTION[cmp_op] + (int)CYC_INSTRUCTION[branch_op];
    remain = GET_CYCLES();
    if (loop_cycles <= 0 || remain < loop_cycles) return;
    max_n = (uint32_t)(remain / loop_cycles);
    if (!max_n) return;

    while (n < max_n && addr <= 0x00bffffeu) {
        uint16_t src16;
        uint32_t src, res;
        __builtin_memcpy(&src16, MEM + addr, sizeof(src16));
        src = (uint32_t)src16;
        res = dst - src;

        /* BHI after CMP is exactly unsigned dst > src.  Do not consume the
         * first not-taken iteration: normal Musashi will perform that CMP/BHI. */
        if (dst <= src) break;

        last_src = src;
        last_res = res;
        addr += 2u;
        ++n;
    }
    if (!n) return;

    REG_A[areg] = addr;
    FLAG_N = NFLAG_16(last_res);
    FLAG_Z = MASK_OUT_ABOVE_16(last_res);
    FLAG_V = VFLAG_SUB_16(last_src, dst, last_res);
    FLAG_C = CFLAG_16(last_res);
    BusErrFlag = 0;
    USE_CYCLES((int)(n * (uint32_t)loop_cycles));

}

void m68k_tab5_cmphi617_stats(unsigned int *calls, unsigned long long *loops, unsigned int *maxbatch)
{
    if (calls) *calls = 0u;
    if (loops) *loops = 0u;
    if (maxbatch) *maxbatch = 0u;
}
/* R139A6 production-clean: retired BE01 code-window diagnostic physically removed. */


/* Build 6.15h17R20: corrected exact MDX hot-block executor for the dominant RAM-resident
 * loop measured by the R15 profiler.  This is deliberately not a new decoder
 * and not a semantic reimplementation of the 68000 instructions.  The block
 * only removes repeated opcode fetch/dispatch/classification: each non-branch
 * instruction still runs the authoritative Musashi generated handler.  Bcc
 * and DBF reuse the already-validated inline cores above, and the inner
 * CMP.W (An)+ / BHI search reuses CMPHI617.
 *
 * Exact code, including the DBF extension word:
 *   $1993BC 3019 672E 337C 0000 FFFE 4A2E 12FA 670C
 *   $1993CC 7200 122A 0001 D241 D074 1000 204B B058
 *   $1993DC 62FC 2008 908B E248 5340 B02A 0001 6502
 *   $1993EC 1480 5C8A 51CF FFCA
 *
 * Safety gates:
 *   - exact 56-byte signature in ordinary X68000 RAM on every entry;
 *   - T0/T1 trace disabled;
 *   - cycles checked after every architectural instruction;
 *   - generated handlers remain authoritative for all data accesses/errors;
 *   - maximum 64 outer iterations per host invocation, then return to the
 *     ordinary execute loop even if the DBF continues.
 */
#define TAB5_MDX619_PC_START 0x001993bcu
#define TAB5_MDX619_PC_END   0x001993f4u
#define TAB5_MDX619_MAX_OUTER 64u

typedef struct {
    uint16_t op;
    uint8_t cycles;
    uint8_t _pad;
    tab5_m68k_handler_t handler;
} tab5_mdx619_fixed_t;

enum {
    MDX619_3019 = 0, MDX619_337C, MDX619_4A2E, MDX619_7200,
    MDX619_122A, MDX619_D241, MDX619_D074, MDX619_204B,
    MDX619_B058, MDX619_2008, MDX619_908B, MDX619_E248,
    MDX619_5340, MDX619_B02A, MDX619_1480, MDX619_5C8A,
    MDX619_FIXED_COUNT
};

static DRAM_ATTR tab5_mdx619_fixed_t s_tab5_mdx619_fixed[MDX619_FIXED_COUNT];
static DRAM_ATTR uint8_t s_tab5_mdx619_cache_ready = 0;
static DRAM_ATTR uint8_t s_tab5_mdx619_cyc_672e = 0;
static DRAM_ATTR uint8_t s_tab5_mdx619_cyc_670c = 0;
static DRAM_ATTR uint8_t s_tab5_mdx619_cyc_62fc = 0;
static DRAM_ATTR uint8_t s_tab5_mdx619_cyc_6502 = 0;
static DRAM_ATTR uint8_t s_tab5_mdx619_cyc_51cf = 0;

/* R57E128B: FB127 hot telemetry retired; R127B/R123 CPU path restored. */
static DRAM_ATTR const uint16_t s_tab5_mdx619_signature[28] = {
    0x3019u,0x672eu,0x337cu,0x0000u,0xfffeu,0x4a2eu,0x12fau,0x670cu,
    0x7200u,0x122au,0x0001u,0xd241u,0xd074u,0x1000u,0x204bu,0xb058u,
    0x62fcu,0x2008u,0x908bu,0xe248u,0x5340u,0xb02au,0x0001u,0x6502u,
    0x1480u,0x5c8au,0x51cfu,0xffcau
};

static inline __attribute__((always_inline)) int tab5_r127_mdx_signature_ok(void)
{
    const int diff = memcmp(MEM + TAB5_MDX619_PC_START, s_tab5_mdx619_signature,
                            sizeof(s_tab5_mdx619_signature));
    return diff == 0;
}

static inline __attribute__((always_inline))
void tab5_mdx619_cache_init(void)
{
    static const uint16_t ops[MDX619_FIXED_COUNT] = {
        0x3019u,0x337cu,0x4a2eu,0x7200u,0x122au,0xd241u,0xd074u,0x204bu,
        0xb058u,0x2008u,0x908bu,0xe248u,0x5340u,0xb02au,0x1480u,0x5c8au
    };
    unsigned i;
    if (s_tab5_mdx619_cache_ready) return;
    for (i = 0; i < MDX619_FIXED_COUNT; ++i) {
        const uint16_t op = ops[i];
        s_tab5_mdx619_fixed[i].op = op;
        s_tab5_mdx619_fixed[i].cycles = (uint8_t)CYC_INSTRUCTION[op];
        s_tab5_mdx619_fixed[i].handler = m68ki_instruction_jump_table[op];
    }
    s_tab5_mdx619_cyc_672e = (uint8_t)CYC_INSTRUCTION[0x672eu];
    s_tab5_mdx619_cyc_670c = (uint8_t)CYC_INSTRUCTION[0x670cu];
    s_tab5_mdx619_cyc_62fc = (uint8_t)CYC_INSTRUCTION[0x62fcu];
    s_tab5_mdx619_cyc_6502 = (uint8_t)CYC_INSTRUCTION[0x6502u];
    s_tab5_mdx619_cyc_51cf = (uint8_t)CYC_INSTRUCTION[0x51cfu];
    s_tab5_mdx619_cache_ready = 1;
}

static inline __attribute__((always_inline))
uint8_t tab5_mdx619_bcc_cycles(uint16_t op)
{
    if (op == 0x672eu) return s_tab5_mdx619_cyc_672e;
    if (op == 0x670cu) return s_tab5_mdx619_cyc_670c;
    if (op == 0x62fcu) return s_tab5_mdx619_cyc_62fc;
    if (op == 0x6502u) return s_tab5_mdx619_cyc_6502;
    return (uint8_t)CYC_INSTRUCTION[op];
}

static inline __attribute__((always_inline))
int tab5_mdx619_ram8(uint32_t a)
{
    return ((a & 0xff000000u) == 0u && a <= 0x00bfffffu);
}

static inline __attribute__((always_inline))
int tab5_mdx619_ram16(uint32_t a)
{
    return ((a & 0xff000001u) == 0u && a <= 0x00bffffeu);
}

static inline __attribute__((always_inline))
int tab5_mdx619_code_overlap(uint32_t a, uint32_t bytes)
{
    const uint32_t e = a + bytes;
    return !(e <= TAB5_MDX619_PC_START || a >= TAB5_MDX619_PC_END);
}

/* Build 6.15h17R22: dominant zero-entry run accelerator.
 *
 * R20 profiling showed the exact $1993BC block averaging only ~4.5 guest
 * instructions per outer iteration.  The zero-entry path is exactly four:
 *
 *   MOVE.W (A1)+,D0 ; BEQ $1993EE ; ADDQ.L #6,A2 ; DBF D7,$1993BC
 *
 * Therefore well over 90% of measured outer iterations take this path.
 * Execute complete zero iterations as one scheduler-safe host loop instead of
 * re-entering four architectural handlers per item.  We never cross the
 * current m68k_execute() cycle boundary: only iterations whose complete 36
 * cycles (40 on terminal DBF) fit inside GET_CYCLES() are batched.  A partial
 * final iteration is left to the exact R20/stock path.
 */

static inline __attribute__((always_inline)) uint16_t tab5_mdx622_load16(uint32_t a)
{
    uint16_t v;
    __builtin_memcpy(&v, MEM + a, sizeof(v));
    BusErrFlag = 0;
    return v;
}

static IRAM_ATTR __attribute__((noinline, hot, optimize("O3")))
int tab5_mdx622_zero_run_try(void)
{
    uint32_t batch = 0u;
    int executed_any = 0;

    if (CPU_TYPE != CPU_TYPE_000) return 0;
    if (FLAG_T1 || FLAG_T0) return 0;
    if ((REG_PC & 0x00ffffffu) != TAB5_MDX619_PC_START) return 0;
    if (GET_CYCLES() <= 0) return 0;

    /* Same self-modifying-code safety gate as R20. */
    if (!tab5_r127_mdx_signature_ok()) return 0;

    m68ki_use_data_space();
    m68ki_set_fc(FLAG_S | FUNCTION_CODE_USER_PROGRAM);

    while (batch < TAB5_MDX619_MAX_OUTER &&
           (REG_PC & 0x00ffffffu) == TAB5_MDX619_PC_START) {
        const uint32_t a1 = REG_A[1];
        const uint16_t d7 = (uint16_t)(REG_D[7] & 0xffffu);
        const int iter_cycles = (d7 == 0u) ? 40 : 36;

        /* Do not cross the scheduler boundary.  If the remaining budget cannot
         * contain a whole zero iteration, leave it to R20/stock so the exact
         * instruction at which the timeslice expires is preserved. */
        if (GET_CYCLES() < iter_cycles)
            break;
        if (!tab5_mdx619_ram16(a1))
            break;

        const uint16_t w = tab5_mdx622_load16(a1);
        if (w != 0u)
            break;

        /* MOVE.W (A1)+,D0 with zero result.  X is unaffected. */
        REG_A[1] = a1 + 2u;
        REG_D[0] = MASK_OUT_BELOW_16(REG_D[0]); /* low word = 0 */
        FLAG_N = NFLAG_CLEAR;
        FLAG_Z = 0u;
        FLAG_V = VFLAG_CLEAR;
        FLAG_C = CFLAG_CLEAR;

        /* BEQ is taken. ADDQ.L #6,A2 targets An and therefore leaves CCR
         * untouched.  DBF also leaves CCR untouched. */
        REG_A[2] = MASK_OUT_ABOVE_32(REG_A[2] + 6u);
        REG_D[7] = MASK_OUT_BELOW_16(REG_D[7]) |
                   (uint32_t)((uint16_t)(d7 - 1u));

        REG_PPC = 0x001993f0u;
        REG_IR  = 0x51cfu;
        REG_PC  = (d7 == 0u) ? TAB5_MDX619_PC_END : TAB5_MDX619_PC_START;
        USE_CYCLES(iter_cycles);

        executed_any = 1;
        ++batch;

        if (d7 == 0u || GET_CYCLES() <= 0)
            break;
    }

    return executed_any;
}

void m68k_tab5_mdx622_stats(unsigned int *calls, unsigned long long *loops, unsigned int *maxbatch)
{
    if (calls) *calls = 0u;
    if (loops) *loops = 0u;
    if (maxbatch) *maxbatch = 0u;
}

/* Execute one fixed non-branch instruction exactly as the normal Musashi loop
 * would after opcode fetch.  The opcode itself is signature-proven ordinary
 * RAM, so skipping only that fetch cannot hide an address/bus error. */
static inline __attribute__((always_inline))
int tab5_mdx619_fixed_step(uint32_t pc, unsigned slot, uint32_t expected_next)
{
    const tab5_mdx619_fixed_t * const f = &s_tab5_mdx619_fixed[slot];
    if (GET_CYCLES() <= 0) return 0;
    m68ki_use_data_space();
    m68ki_instr_hook(pc);
    REG_PPC = pc;
    m68ki_set_fc(FLAG_S | FUNCTION_CODE_USER_PROGRAM);
    BusErrFlag = 0;
    REG_IR = f->op;
    REG_PC = pc + 2u;
    f->handler();
    USE_CYCLES((int)f->cycles);
    return ((REG_PC & 0x00ffffffu) == expected_next);
}

static inline __attribute__((always_inline))
int tab5_mdx619_bcc_step(uint32_t pc, uint16_t op, int taken, uint32_t expected_next)
{
    if (GET_CYCLES() <= 0) return 0;
    m68ki_use_data_space();
    m68ki_instr_hook(pc);
    REG_PPC = pc;
    m68ki_set_fc(FLAG_S | FUNCTION_CODE_USER_PROGRAM);
    BusErrFlag = 0;
    REG_IR = op;
    REG_PC = pc + 2u;
    tab5_bcc598f_exec(op, taken);
    USE_CYCLES((int)tab5_mdx619_bcc_cycles(op));
    return ((REG_PC & 0x00ffffffu) == expected_next);
}

static inline __attribute__((always_inline))
int tab5_mdx619_dbf_step(uint32_t *next_pc)
{
    const uint32_t pc = 0x001993f0u;
    const uint16_t op = 0x51cfu;
    if (GET_CYCLES() <= 0) return 0;
    m68ki_use_data_space();
    m68ki_instr_hook(pc);
    REG_PPC = pc;
    m68ki_set_fc(FLAG_S | FUNCTION_CODE_USER_PROGRAM);
    BusErrFlag = 0;
    REG_IR = op;
    REG_PC = pc + 2u;
    (void)tab5_dbcc598f_exec(op);
    USE_CYCLES((int)s_tab5_mdx619_cyc_51cf);
    *next_pc = REG_PC & 0x00ffffffu;
    return (*next_pc == TAB5_MDX619_PC_START || *next_pc == TAB5_MDX619_PC_END);
}

static IRAM_ATTR __attribute__((noinline, hot, optimize("O3")))
int tab5_mdx619_try(void)
{
    uint32_t outer_batch = 0u;
    int executed_any = 0;

    if (FLAG_T1 || FLAG_T0) return 0;
    if ((REG_PC & 0x00ffffffu) != TAB5_MDX619_PC_START) return 0;
    if (GET_CYCLES() <= 0) return 0;

    /* Complete signature verification on every block entry.  The 56-byte
     * memcmp is much cheaper than the opcode-dispatch traffic it replaces and
     * ensures self-modifying or different guest code immediately falls back. */
    if (!tab5_r127_mdx_signature_ok()) return 0;

    tab5_mdx619_cache_init();

    while (outer_batch < TAB5_MDX619_MAX_OUTER && GET_CYCLES() > 0 &&
           (REG_PC & 0x00ffffffu) == TAB5_MDX619_PC_START) {
        uint32_t next_pc;
        int taken;

        /* MOVE.W (A1)+,D0.  R19 deliberately accelerates this block only
         * while every measured data operand stays in ordinary X68000 RAM. */
        if (!tab5_mdx619_ram16(REG_A[1])) return 0;
        if (!tab5_mdx619_fixed_step(0x001993bcu, MDX619_3019, 0x001993beu)) return 1;
        executed_any = 1;
        if (GET_CYCLES() <= 0) return 1;

        /* BEQ.B $1993EE */
        taken = (FLAG_Z == 0u);
        if (!tab5_mdx619_bcc_step(0x001993beu, 0x672eu, taken,
                                  taken ? 0x001993eeu : 0x001993c0u)) return 1;
        if (GET_CYCLES() <= 0) return 1;
        if (!taken) {
            {
                const uint32_t wr = REG_A[1] - 2u;
                if (!tab5_mdx619_ram16(wr) || tab5_mdx619_code_overlap(wr, 2u)) return 1;
            }
            if (!tab5_mdx619_fixed_step(0x001993c0u, MDX619_337C, 0x001993c6u)) return 1;
            if (GET_CYCLES() <= 0) return 1;
            if (!tab5_mdx619_ram8(REG_A[6] + 0x12fau)) return 1;
            if (!tab5_mdx619_fixed_step(0x001993c6u, MDX619_4A2E, 0x001993cau)) return 1;
            if (GET_CYCLES() <= 0) return 1;

            /* BEQ.B $1993D8 */
            taken = (FLAG_Z == 0u);
            if (!tab5_mdx619_bcc_step(0x001993cau, 0x670cu, taken,
                                      taken ? 0x001993d8u : 0x001993ccu)) return 1;
            if (GET_CYCLES() <= 0) return 1;
            if (!taken) {
                if (!tab5_mdx619_fixed_step(0x001993ccu, MDX619_7200, 0x001993ceu)) return 1;
                if (GET_CYCLES() <= 0) return 1;
                if (!tab5_mdx619_ram8(REG_A[2] + 1u)) return 1;
                if (!tab5_mdx619_fixed_step(0x001993ceu, MDX619_122A, 0x001993d2u)) return 1;
                if (GET_CYCLES() <= 0) return 1;
                if (!tab5_mdx619_fixed_step(0x001993d2u, MDX619_D241, 0x001993d4u)) return 1;
                if (GET_CYCLES() <= 0) return 1;
                {
                    const uint32_t iea = REG_A[4] + (uint32_t)(int32_t)(int16_t)(REG_D[1] & 0xffffu);
                    if (!tab5_mdx619_ram16(iea)) return 1;
                }
                if (!tab5_mdx619_fixed_step(0x001993d4u, MDX619_D074, 0x001993d8u)) return 1;
                if (GET_CYCLES() <= 0) return 1;
            }

            if (!tab5_mdx619_ram16(REG_A[3])) return 1;
            if (!tab5_mdx619_fixed_step(0x001993d8u, MDX619_204B, 0x001993dau)) return 1;
            if (GET_CYCLES() <= 0) return 1;

            /* Inner threshold scan: execute the first CMP/BHI architecturally,
             * then let the proven CMPHI617 helper fold additional taken pairs. */
            for (;;) {
                if (!tab5_mdx619_fixed_step(0x001993dau, MDX619_B058, 0x001993dcu)) return 1;
                if (GET_CYCLES() <= 0) return 1;
                taken = tab5_bcc598f_taken(0x62fcu);
                if (!tab5_mdx619_bcc_step(0x001993dcu, 0x62fcu, taken,
                                          taken ? 0x001993dau : 0x001993deu)) return 1;
                if (GET_CYCLES() <= 0) return 1;
                if (!taken) break;
                tab5_cmphi617_try(0x62fcu);
                if (GET_CYCLES() <= 0) return 1;
                if ((REG_PC & 0x00ffffffu) != 0x001993dau) return 1;
            }

            if (!tab5_mdx619_fixed_step(0x001993deu, MDX619_2008, 0x001993e0u)) return 1;
            if (GET_CYCLES() <= 0) return 1;
            if (!tab5_mdx619_fixed_step(0x001993e0u, MDX619_908B, 0x001993e2u)) return 1;
            if (GET_CYCLES() <= 0) return 1;
            if (!tab5_mdx619_fixed_step(0x001993e2u, MDX619_E248, 0x001993e4u)) return 1;
            if (GET_CYCLES() <= 0) return 1;
            if (!tab5_mdx619_fixed_step(0x001993e4u, MDX619_5340, 0x001993e6u)) return 1;
            if (GET_CYCLES() <= 0) return 1;
            if (!tab5_mdx619_ram8(REG_A[2] + 1u)) return 1;
            if (!tab5_mdx619_fixed_step(0x001993e6u, MDX619_B02A, 0x001993eau)) return 1;
            if (GET_CYCLES() <= 0) return 1;

            /* BCS.B skips MOVE.B D0,(A2). */
            taken = tab5_bcc598f_taken(0x6502u);
            if (!tab5_mdx619_bcc_step(0x001993eau, 0x6502u, taken,
                                      taken ? 0x001993eeu : 0x001993ecu)) return 1;
            if (GET_CYCLES() <= 0) return 1;
            if (!taken) {
                if (!tab5_mdx619_ram8(REG_A[2]) || tab5_mdx619_code_overlap(REG_A[2], 1u)) return 1;
                if (!tab5_mdx619_fixed_step(0x001993ecu, MDX619_1480, 0x001993eeu)) return 1;
                if (GET_CYCLES() <= 0) return 1;
            }
        }

        if (!tab5_mdx619_fixed_step(0x001993eeu, MDX619_5C8A, 0x001993f0u)) return 1;
        if (GET_CYCLES() <= 0) return 1;
        if (!tab5_mdx619_dbf_step(&next_pc)) return 1;
        ++outer_batch;
        if (GET_CYCLES() <= 0) break;
        if (next_pc != TAB5_MDX619_PC_START) break;
    }

    return executed_any;
}

void m68k_tab5_mdx619_stats(unsigned int *calls, unsigned long long *outer,
                              unsigned long long *fixed_insn, unsigned int *maxbatch)
{
    if (calls) *calls = 0u;
    if (outer) *outer = 0u;
    if (fixed_insn) *fixed_insn = 0u;
    if (maxbatch) *maxbatch = 0u;
}



/* Build 5.98g11: collapse the exact sparse-clear loop measured in the g10
 * SFXVI profile.  One architectural iteration, including the taken BPL, has
 * already completed when this helper runs:
 *
 *     loop: CLR.W  (A0)+
 *           CLR.W  (A0)+
 *           SUBQ.W #1,D0
 *           ADDQ   #4,A0       ; address-register quick does not change CCR
 *           BPL.B  loop        ; $6AF6, ten-byte loop
 *
 * Each taken iteration clears four consecutive bytes and advances A0 by eight
 * bytes.  Future taken iterations are side-effect-free when the destination is
 * ordinary RAM.  Batch only whole taken iterations that fit inside the current
 * Musashi scheduler slice and leave the final N=1/not-taken iteration to stock
 * Musashi.  This preserves the scheduler/IRQ observation point and exact exit
 * behavior while eliminating five guest dispatches per batched iteration. */
static inline __attribute__((always_inline))
void tab5_sparse598g11_try_bpl(uint16_t branch_op)
{
    const uint32_t branch_pc = REG_PPC & 0x00ffffffu;
    const uint32_t loop_pc = REG_PC & 0x00ffffffu;
    const uint16_t op_clr = 0x4258u;
    const uint16_t op_sub = 0x5340u;
    const uint16_t op_add = 0x5848u;
    const uint16_t op_bpl = 0x6af6u;
    uint32_t a, counter, future_taken, max_n, max_ram, n, span, i;
    int loop_cycles, remain;
    const uint16_t zero16 = 0u;

    if (FLAG_T1 || FLAG_T0) return;
    if (branch_op != op_bpl) return;
    /* Taken BPL leaves REG_PC at loop_pc.  For the exact -10 displacement the
     * branch opcode itself is eight bytes after the loop start. */
    if (branch_pc != ((loop_pc + 8u) & 0x00ffffffu)) return;
    if (tab5_poll58_fetch16(loop_pc + 0u) != op_clr ||
        tab5_poll58_fetch16(loop_pc + 2u) != op_clr ||
        tab5_poll58_fetch16(loop_pc + 4u) != op_sub ||
        tab5_poll58_fetch16(loop_pc + 6u) != op_add ||
        tab5_poll58_fetch16(loop_pc + 8u) != op_bpl) return;

    /* The current taken iteration already decremented D0.w.  A value k in
     * 0..32767 means exactly k more taken iterations remain before the final
     * subtraction 0 -> FFFF makes N=1 and exits. */
    counter = REG_D[0] & 0xffffu;
    if (counter == 0u || (counter & 0x8000u)) return;
    future_taken = counter;

    loop_cycles = CYC_INSTRUCTION[op_clr] * 2 + CYC_INSTRUCTION[op_sub] +
                  CYC_INSTRUCTION[op_add] + CYC_INSTRUCTION[op_bpl];
    remain = GET_CYCLES();
    if (loop_cycles <= 0 || remain < loop_cycles) return;
    max_n = (uint32_t)(remain / loop_cycles);
    if (!max_n) return;

    a = REG_A[0];
    /* Preserve the full architectural address: no aliasing, odd access, MMIO,
     * or wrap is allowed on the elided path.  Conservatively leave A0 pointing
     * at an ordinary-RAM address for the next normal iteration as well. */
    if ((a & 0xff000001u) || a > 0x00bffffcu) return;
    max_ram = (0x00bffffcu - a) >> 3;
    if (!max_ram) return;

    n = future_taken;
    if (n > max_n) n = max_n;
    if (n > max_ram) n = max_ram;
    if (!n) return;
    span = n << 3;

    /* Reject self-modifying overlap.  The bound includes the four-byte holes,
     * which is intentionally conservative and therefore safe. */
    if (!(a + span <= loop_pc || a >= branch_pc + 2u)) return;

    /* Zero two 16-bit words per iteration.  PX68K RAM is host word-swapped,
     * but zero is endian-invariant.  16-bit memcpy keeps the runtime even-word
     * alignment contract explicit on RISC-V without assuming 32-bit alignment. */
    for (i = 0; i < n; ++i) {
        const uint32_t p = a + (i << 3);
        __builtin_memcpy(MEM + p, &zero16, 2u);
        __builtin_memcpy(MEM + p + 2u, &zero16, 2u);
    }
    BusErrFlag = 0;
    m68k_tab5_exec123_invalidate_range(a, span);

    REG_A[0] = a + span;
    counter -= n;
    REG_D[0] = (REG_D[0] & 0xffff0000u) | counter;

    /* Exact flags after the last elided SUBQ.W #1,D0.  Because every elided
     * iteration is a taken BPL iteration, the subtraction cannot borrow or
     * overflow and its result is 0..32767.  ADDQ to An and BPL leave CCR as-is. */
    FLAG_N = NFLAG_16(counter);
    FLAG_Z = counter;
    FLAG_V = VFLAG_CLEAR;
    FLAG_X = FLAG_C = CFLAG_CLEAR;


    USE_CYCLES((int)(n * (uint32_t)loop_cycles));


}

/* Build 5.98g6: collapse the exact hot RAM-clear loop observed in SFXVI:
 *
 *     loop: CLR.L  (An)+
 *           CMPA.L Am,An
 *           BNE.B  loop
 *
 * The current BNE has already executed architecturally and was taken.  Future
 * taken iterations have no externally visible device access when the cleared
 * range is ordinary RAM.  Batch only whole taken iterations that fit inside
 * the current Musashi slice and leave the final equality/not-taken iteration
 * to normal Musashi.  CCR is reconstructed exactly as the last elided CMPA.L;
 * X is untouched by both CLR and CMPA. */
static inline __attribute__((always_inline))
void tab5_clear598g6_try_cmpa_bne(uint16_t branch_op)
{
    const uint32_t branch_pc = REG_PPC & 0x00ffffffu;
    const uint32_t loop_pc = REG_PC & 0x00ffffffu;
    uint16_t clr, cmpa;
    unsigned dst_a, cmp_dst_a, end_a;
    uint32_t a, end, delta, total_remaining, future_taken;
    uint32_t max_n, n, bytes, new_a, res;
    int loop_cycles, remain;

    /* Build 5.98g6: this accelerator is profile-transparent.  Earlier g5
     * disabled batching while either profiler was active, which meant the
     * exact $4298/$B1C9/$66FA loop was visible in profiles but never actually
     * exercised by the accelerator in those sampled frames.  Logical opcode
     * counts are credited below for elided iterations instead. */
    if (FLAG_T1 || FLAG_T0) return;
    if ((branch_op & 0xff00u) != 0x6600u || (branch_op & 0x00ffu) == 0u) return;
    if ((int8_t)(branch_op & 0xffu) != -6) return;
    if (branch_pc != ((loop_pc + 4u) & 0x00ffffffu)) return;

    clr = tab5_poll58_fetch16(loop_pc);
    cmpa = tab5_poll58_fetch16(loop_pc + 2u);
    if ((clr & 0xfff8u) != 0x4298u) return;   /* CLR.L (An)+ */
    if ((cmpa & 0xf1f8u) != 0xb1c8u) return;  /* CMPA.L Am,An */

    dst_a = clr & 7u;
    cmp_dst_a = (cmpa >> 9) & 7u;
    end_a = cmpa & 7u;
    if (cmp_dst_a != dst_a || end_a == dst_a) return;

    a = REG_A[dst_a];
    end = REG_A[end_a];
    if ((a | end) & 0xff000000u) return; /* no 24-bit alias/wrap */
    if ((a & 1u) || a > 0x00bfffffu || end > 0x00c00000u || end <= a) return;
    delta = end - a;
    if (delta & 3u) return;

    total_remaining = delta >> 2; /* includes the final not-taken iteration */
    if (total_remaining <= 1u) return;
    future_taken = total_remaining - 1u;

    loop_cycles = CYC_INSTRUCTION[clr] + CYC_INSTRUCTION[cmpa] + CYC_INSTRUCTION[branch_op];
    remain = GET_CYCLES();
    if (loop_cycles <= 0 || remain < loop_cycles) return;
    max_n = (uint32_t)(remain / loop_cycles);
    n = (future_taken < max_n) ? future_taken : max_n;
    if (!n) return;

    bytes = n << 2;
    if (bytes > (0x00c00000u - a)) return;
    /* Elided writes must not modify the code words we are about to re-enter. */
    if (!(a + bytes <= loop_pc || a >= branch_pc + 2u)) return;
    if (!px68k_m68k_zero_fill_ram(a, bytes)) return;

    new_a = a + bytes;
    REG_A[dst_a] = new_a;

    /* Exact CMPA.L Am,An flags after the last elided taken iteration. */
    res = new_a - end;
    FLAG_N = NFLAG_32(res);
    FLAG_Z = MASK_OUT_ABOVE_32(res);
    FLAG_V = VFLAG_SUB_32(end, new_a, res);
    FLAG_C = CFLAG_SUB_32(end, new_a, res);


    USE_CYCLES((int)(n * (uint32_t)loop_cycles));


}


/* Build 5.73: collapse a pure DBF self-loop.
 *
 *     loop: DBF Dn,loop
 *
 * One taken iteration has already executed normally when this helper runs.
 * Future taken iterations only decrement Dn.w; DBF does not modify CCR.
 * Batch only within the current Musashi slice and leave the final exit to
 * normal Musashi so scheduler/IRQ observation points stay unchanged. */
static inline __attribute__((always_inline)) void tab5_dbf73_try_self(uint16_t dbf_op)
{
    const uint32_t branch_pc = REG_PPC & 0x00ffffffu;
    const uint32_t loop_pc = REG_PC & 0x00ffffffu;
    unsigned cnt_d;
    uint32_t counter, max_n, n;
    int per_taken, remain;

    if (FLAG_T1 || FLAG_T0) return;
    if (loop_pc != branch_pc) return; /* overwhelmingly common rejection */

    cnt_d = dbf_op & 7u;
    counter = REG_D[cnt_d] & 0xffffu;
    if (counter == 0u || counter == 0xffffu) return;

    per_taken = CYC_INSTRUCTION[dbf_op] + CYC_DBCC_F_NOEXP;
    remain = GET_CYCLES();
    if (per_taken <= 0 || remain < per_taken) return;

    max_n = (uint32_t)(remain / per_taken);
    n = (counter < max_n) ? counter : max_n;
    if (n == 0u) return;

    REG_D[cnt_d] = (REG_D[cnt_d] & 0xffff0000u) | ((counter - n) & 0xffffu);
    USE_CYCLES((int)(n * (uint32_t)per_taken));

}


/* Build 5.74: collapse a short DBF body made only of repeated identical
 * register-to-postincrement stores:
 *
 *     loop: MOVE.W Dn,(An)+    x 1..23
 *           DBF    Dm,loop
 *
 * or the corresponding MOVE.L form.  One complete taken iteration has
 * already executed normally.  Therefore REG_A points at the next contiguous
 * destination and REG_D[Dm].w is the remaining DBF counter.  With Dn != Dm,
 * future body iterations write one invariant value, leave the same final CCR
 * as the already executed body, and DBF itself does not alter CCR.
 *
 * We batch only WHOLE future taken iterations that fit inside the current
 * Musashi timeslice.  This preserves the existing 200-cycle PX68K scheduler
 * boundary exactly; the final DBF exit remains normal Musashi execution.
 */
#define TAB5_DBF74_MAX_BODY_OPS 23u
static inline __attribute__((always_inline)) int tab5_dbf74_try_store_body(uint16_t dbf_op)
{
    const uint32_t branch_pc = REG_PPC & 0x00ffffffu;
    const uint32_t loop_pc = REG_PC & 0x00ffffffu;
    uint32_t body_bytes, body_ops, i;
    uint16_t mov, disp_u;
    int32_t disp;
    unsigned src_d, dst_a, cnt_d;
    uint32_t unit_bytes, value, counter, max_n, n;
    uint32_t a, logical_stores, bytes;
    int move_cycles, dbf_taken_cycles, loop_cycles, remain;

    if ((dbf_op & 0xfff8u) != 0x51c8u || FLAG_T1 || FLAG_T0) return 0;
    if (loop_pc >= branch_pc) return 0;

    body_bytes = branch_pc - loop_pc;
    if ((body_bytes & 1u) || body_bytes < 2u) return 0;
    body_ops = body_bytes >> 1;
    if (body_ops > TAB5_DBF74_MAX_BODY_OPS) return 0;

    mov = tab5_poll58_fetch16(loop_pc);
    if ((mov & 0xf1f8u) == 0x30c0u)      unit_bytes = 2u; /* MOVE.W Dn,(An)+ */
    else if ((mov & 0xf1f8u) == 0x20c0u) unit_bytes = 4u; /* MOVE.L Dn,(An)+ */
    else return 0;

    for (i = 1u; i < body_ops; ++i)
        if (tab5_poll58_fetch16(loop_pc + (i << 1)) != mov) return 0;

    /* Verify the architectural DBF displacement rather than trusting REG_PC
     * alone; this rejects unusual control-flow/state-rewrite cases. */
    disp_u = tab5_poll58_fetch16(branch_pc + 2u);
    disp = (int16_t)disp_u;
    if ((((uint32_t)((int32_t)(branch_pc + 2u) + disp)) & 0x00ffffffu) != loop_pc) return 0;

    src_d = mov & 7u;
    dst_a = (mov >> 9) & 7u;
    cnt_d = dbf_op & 7u;
    if (src_d == cnt_d) return 0; /* source would change at every DBF */

    counter = REG_D[cnt_d] & 0xffffu;
    if (counter == 0u || counter == 0xffffu) return 0;

    move_cycles = CYC_INSTRUCTION[mov];
    dbf_taken_cycles = CYC_INSTRUCTION[dbf_op] + CYC_DBCC_F_NOEXP;
    loop_cycles = move_cycles * (int)body_ops + dbf_taken_cycles;
    remain = GET_CYCLES();
    if (move_cycles <= 0 || dbf_taken_cycles <= 0 || loop_cycles <= 0 || remain < loop_cycles)
        return 0;

    max_n = (uint32_t)(remain / loop_cycles);
    n = (counter < max_n) ? counter : max_n;
    if (n == 0u) return 0;

    a = REG_A[dst_a];
    if ((a & 0xff000001u) != 0u || a > 0x00bfffffu) return 0;
    logical_stores = n * body_ops; /* bounded: current slice, body <=23 */
    bytes = logical_stores * unit_bytes;
    if (bytes > (0x00c00000u - a)) return 0;

    /* A future store must not rewrite any body/DBF word that is being
     * elided; otherwise sequential execution could fetch modified code. */
    if (!(a + bytes <= loop_pc || a >= branch_pc + 4u)) return 0;

    value = (unit_bytes == 2u) ? (REG_D[src_d] & 0xffffu) : REG_D[src_d];
    if (!px68k_m68k_repeat_fill_ram(a, value, unit_bytes, logical_stores)) return 0;

    REG_A[dst_a] = a + bytes;
    REG_D[cnt_d] = (REG_D[cnt_d] & 0xffff0000u) | ((counter - n) & 0xffffu);
    USE_CYCLES((int)(n * (uint32_t)loop_cycles));



    return 1;
}


/* Build 5.59: collapse the exact RAM zero-fill micro-loop observed on real
 * X68000 code:
 *
 *     MOVE.L Dn,(An)+    x4
 *     DBF    Dm,loop
 *
 * The optimization is deliberately dynamic and conservative.  It requires
 * four identical MOVE.L opcodes, a DBF target exactly back to the first MOVE,
 * a zero source register, distinct source/counter D registers, no trace mode,
 * and a destination range wholly inside ordinary X68000 RAM.  We batch only
 * future *taken* DBF iterations that fit in the current Musashi timeslice;
 * the final DBF exit remains normal Musashi execution.  Thus peripheral
 * scheduler granularity and architectural flags/PC semantics are preserved.
 */
static inline __attribute__((always_inline)) void tab5_fill59_try_dbf(uint16_t dbf_op)
{
    uint32_t branch_pc, loop_pc, a, bytes, n, max_n, counter;
    uint16_t mov, disp_u;
    int32_t disp;
    unsigned src_d, dst_a, cnt_d;
    int move_cycles, dbf_taken_cycles, loop_cycles, remain;

    if ((dbf_op & 0xfff8u) != 0x51c8u) return; /* DBF only */
    if (FLAG_T1 || FLAG_T0) return;

    branch_pc = REG_PPC & 0x00ffffffu;
    loop_pc = REG_PC & 0x00ffffffu;
    if (loop_pc >= branch_pc || (branch_pc - loop_pc) != 8u) return;

    mov = tab5_poll58_fetch16(loop_pc);
    if ((mov & 0xf1f8u) != 0x20c0u) return; /* MOVE.L Dn,(An)+ */
    if (tab5_poll58_fetch16(loop_pc + 2u) != mov ||
        tab5_poll58_fetch16(loop_pc + 4u) != mov ||
        tab5_poll58_fetch16(loop_pc + 6u) != mov) return;

    disp_u = tab5_poll58_fetch16(branch_pc + 2u);
    disp = (int16_t)disp_u;
    if ((((uint32_t)((int32_t)(branch_pc + 2u) + disp)) & 0x00ffffffu) != loop_pc) return;

    src_d = mov & 7u;
    dst_a = (mov >> 9) & 7u;
    cnt_d = dbf_op & 7u;
    if (src_d == cnt_d || REG_D[src_d] != 0u) return;

    /* We are called only after a taken DBF.  The low word is therefore the
     * remaining counter value; batching at most that many iterations leaves
     * the final counter==0 iteration/exit to ordinary Musashi. */
    counter = REG_D[cnt_d] & 0xffffu;
    if (counter == 0u || counter == 0xffffu) return;

    move_cycles = CYC_INSTRUCTION[mov];
    dbf_taken_cycles = CYC_INSTRUCTION[dbf_op] + CYC_DBCC_F_NOEXP;
    loop_cycles = move_cycles * 4 + dbf_taken_cycles;
    remain = GET_CYCLES();
    if (loop_cycles <= 0 || remain < loop_cycles) return;
    max_n = (uint32_t)(remain / loop_cycles);
    n = (counter < max_n) ? counter : max_n;
    if (n == 0u) return;

    a = REG_A[dst_a];
    if (a & 0xff000000u) return; /* no 24-bit wrap/aliasing */
    bytes = n * 16u;
    if (a > 0x00bfffffu || bytes > (0x00c00000u - a)) return;

    /* Self-modifying overlap would require opcode refetch between iterations. */
    if (!(a + bytes <= loop_pc || a >= branch_pc + 4u)) return;
    if (!px68k_m68k_zero_fill_ram(a, bytes)) return;

    REG_A[dst_a] = a + bytes;
    REG_D[cnt_d] = (REG_D[cnt_d] & 0xffff0000u) | ((counter - n) & 0xffffu);
    USE_CYCLES((int)(n * (uint32_t)loop_cycles));

}

/* R139A6 production-clean: opcode-family/PIE candidate dump helpers physically removed. */

/* R57E127B FIX1: production builds retire the opcode profiler but keep its
 * public control ABI because sparse perf plumbing still references it.
 * This ESP-side no-op is intentionally outside the non-ESP #else below. */
void m68k_tab5_opcode_profile_set(int enabled) { (void)enabled; }

void m68k_tab5_dynarec_dump(void)
{
}
#else
void m68k_tab5_opcode_profile_set(int enabled) { (void)enabled; }
void m68k_tab5_dispatch_profile_set(int enabled) { (void)enabled; }
uint32_t m68k_tab5_profile_storage_bytes(void) { return 0u; }
unsigned int m68k_tab5_dynarec_metadata_bytes(void) { return 0u; }
void m68k_tab5_dynarec_bind(void *arena, unsigned int bytes,
                            int (*sync_fn)(void *addr, unsigned int bytes))
{ (void)arena; (void)bytes; (void)sync_fn; }
void m68k_tab5_dynarec_dump(void) { }
#endif

/* ======================================================================== */
/* ================================= DATA ================================= */
/* ======================================================================== */

/*
 * Build 5.49 (ESP32-P4): move Musashi's per-instruction hot state from
 * ordinary L2 SRAM into the P4 8 KiB zero-wait TCM (called SPM by newer IDF).
 * The PX68K linker fragment still leaves bulk emulator state in PSRAM; only
 * the CPU context and cycle/control words that are touched on virtually every
 * emulated instruction live in TCM. */
#ifdef ESP_PLATFORM
TCM_DRAM_ATTR int  m68ki_initial_cycles;
TCM_DRAM_ATTR int  m68ki_remaining_cycles = 0;         /* Number of clocks remaining */
TCM_DRAM_ATTR uint m68ki_tracing = 0;
TCM_DRAM_ATTR uint m68ki_address_space;
#else
int  m68ki_initial_cycles;
int  m68ki_remaining_cycles = 0;                     /* Number of clocks remaining */
uint m68ki_tracing = 0;
uint m68ki_address_space;
#endif

#ifdef M68K_LOG_ENABLE
const char *const m68ki_cpu_names[] =
{
	"Invalid CPU",
	"M68000",
	"M68010",
	"M68EC020",
	"M68020",
	"M68EC030",
	"M68030",
	"M68EC040",
	"M68LC040",
	"M68040",
	"SCC68070",
};
#endif /* M68K_LOG_ENABLE */

/* The CPU core: all D/A registers, PC, flags and execution state are hot. */
#ifdef ESP_PLATFORM
TCM_DRAM_ATTR __attribute__((aligned(16))) m68ki_cpu_core m68ki_cpu = {0};
#else
m68ki_cpu_core m68ki_cpu = {0};
#endif

#if M68K_EMULATE_ADDRESS_ERROR
#ifdef _BSD_SETJMP_H
sigjmp_buf m68ki_aerr_trap;
#else
jmp_buf m68ki_aerr_trap;
#endif
#endif /* M68K_EMULATE_ADDRESS_ERROR */

uint    m68ki_aerr_address;
uint    m68ki_aerr_write_mode;
uint    m68ki_aerr_fc;


/* Used by shift & rotate instructions */
const uint8 m68ki_shift_8_table[65] =
{
	0x00, 0x80, 0xc0, 0xe0, 0xf0, 0xf8, 0xfc, 0xfe, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
	0xff, 0xff, 0xff, 0xff, 0xff
};
const uint16 m68ki_shift_16_table[65] =
{
	0x0000, 0x8000, 0xc000, 0xe000, 0xf000, 0xf800, 0xfc00, 0xfe00, 0xff00,
	0xff80, 0xffc0, 0xffe0, 0xfff0, 0xfff8, 0xfffc, 0xfffe, 0xffff, 0xffff,
	0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
	0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
	0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
	0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
	0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff, 0xffff,
	0xffff, 0xffff
};
const uint m68ki_shift_32_table[65] =
{
	0x00000000, 0x80000000, 0xc0000000, 0xe0000000, 0xf0000000, 0xf8000000,
	0xfc000000, 0xfe000000, 0xff000000, 0xff800000, 0xffc00000, 0xffe00000,
	0xfff00000, 0xfff80000, 0xfffc0000, 0xfffe0000, 0xffff0000, 0xffff8000,
	0xffffc000, 0xffffe000, 0xfffff000, 0xfffff800, 0xfffffc00, 0xfffffe00,
	0xffffff00, 0xffffff80, 0xffffffc0, 0xffffffe0, 0xfffffff0, 0xfffffff8,
	0xfffffffc, 0xfffffffe, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
	0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
	0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
	0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
	0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff,
	0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff
};


/* Number of clock cycles to use for exception processing.
 * I used 4 for any vectors that are undocumented for processing times.
 */
const uint8 m68ki_exception_cycle_table[5][256] =
{
	{ /* 000 */
		 40, /*  0: Reset - Initial Stack Pointer                      */
		  4, /*  1: Reset - Initial Program Counter                    */
		 50, /*  2: Bus Error                             (unemulated) */
		 50, /*  3: Address Error                         (unemulated) */
		 34, /*  4: Illegal Instruction                                */
		 38, /*  5: Divide by Zero                                     */
		 40, /*  6: CHK                                                */
		 34, /*  7: TRAPV                                              */
		 34, /*  8: Privilege Violation                                */
		 34, /*  9: Trace                                              */
		 34, /* 10: 1010                                               */
		 34, /* 11: 1111                                               */
		  4, /* 12: RESERVED                                           */
		  4, /* 13: Coprocessor Protocol Violation        (unemulated) */
		  4, /* 14: Format Error                                       */
		 44, /* 15: Uninitialized Interrupt                            */
		  4, /* 16: RESERVED                                           */
		  4, /* 17: RESERVED                                           */
		  4, /* 18: RESERVED                                           */
		  4, /* 19: RESERVED                                           */
		  4, /* 20: RESERVED                                           */
		  4, /* 21: RESERVED                                           */
		  4, /* 22: RESERVED                                           */
		  4, /* 23: RESERVED                                           */
		 44, /* 24: Spurious Interrupt                                 */
		 44, /* 25: Level 1 Interrupt Autovector                       */
		 44, /* 26: Level 2 Interrupt Autovector                       */
		 44, /* 27: Level 3 Interrupt Autovector                       */
		 44, /* 28: Level 4 Interrupt Autovector                       */
		 44, /* 29: Level 5 Interrupt Autovector                       */
		 44, /* 30: Level 6 Interrupt Autovector                       */
		 44, /* 31: Level 7 Interrupt Autovector                       */
		 34, /* 32: TRAP #0                                            */
		 34, /* 33: TRAP #1                                            */
		 34, /* 34: TRAP #2                                            */
		 34, /* 35: TRAP #3                                            */
		 34, /* 36: TRAP #4                                            */
		 34, /* 37: TRAP #5                                            */
		 34, /* 38: TRAP #6                                            */
		 34, /* 39: TRAP #7                                            */
		 34, /* 40: TRAP #8                                            */
		 34, /* 41: TRAP #9                                            */
		 34, /* 42: TRAP #10                                           */
		 34, /* 43: TRAP #11                                           */
		 34, /* 44: TRAP #12                                           */
		 34, /* 45: TRAP #13                                           */
		 34, /* 46: TRAP #14                                           */
		 34, /* 47: TRAP #15                                           */
		  4, /* 48: FP Branch or Set on Unknown Condition (unemulated) */
		  4, /* 49: FP Inexact Result                     (unemulated) */
		  4, /* 50: FP Divide by Zero                     (unemulated) */
		  4, /* 51: FP Underflow                          (unemulated) */
		  4, /* 52: FP Operand Error                      (unemulated) */
		  4, /* 53: FP Overflow                           (unemulated) */
		  4, /* 54: FP Signaling NAN                      (unemulated) */
		  4, /* 55: FP Unimplemented Data Type            (unemulated) */
		  4, /* 56: MMU Configuration Error               (unemulated) */
		  4, /* 57: MMU Illegal Operation Error           (unemulated) */
		  4, /* 58: MMU Access Level Violation Error      (unemulated) */
		  4, /* 59: RESERVED                                           */
		  4, /* 60: RESERVED                                           */
		  4, /* 61: RESERVED                                           */
		  4, /* 62: RESERVED                                           */
		  4, /* 63: RESERVED                                           */
		     /* 64-255: User Defined                                   */
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4
	},
	{ /* 010 */
		 40, /*  0: Reset - Initial Stack Pointer                      */
		  4, /*  1: Reset - Initial Program Counter                    */
		126, /*  2: Bus Error                             (unemulated) */
		126, /*  3: Address Error                         (unemulated) */
		 38, /*  4: Illegal Instruction                                */
		 44, /*  5: Divide by Zero                                     */
		 44, /*  6: CHK                                                */
		 34, /*  7: TRAPV                                              */
		 38, /*  8: Privilege Violation                                */
		 38, /*  9: Trace                                              */
		  4, /* 10: 1010                                               */
		  4, /* 11: 1111                                               */
		  4, /* 12: RESERVED                                           */
		  4, /* 13: Coprocessor Protocol Violation        (unemulated) */
		  4, /* 14: Format Error                                       */
		 44, /* 15: Uninitialized Interrupt                            */
		  4, /* 16: RESERVED                                           */
		  4, /* 17: RESERVED                                           */
		  4, /* 18: RESERVED                                           */
		  4, /* 19: RESERVED                                           */
		  4, /* 20: RESERVED                                           */
		  4, /* 21: RESERVED                                           */
		  4, /* 22: RESERVED                                           */
		  4, /* 23: RESERVED                                           */
		 46, /* 24: Spurious Interrupt                                 */
		 46, /* 25: Level 1 Interrupt Autovector                       */
		 46, /* 26: Level 2 Interrupt Autovector                       */
		 46, /* 27: Level 3 Interrupt Autovector                       */
		 46, /* 28: Level 4 Interrupt Autovector                       */
		 46, /* 29: Level 5 Interrupt Autovector                       */
		 46, /* 30: Level 6 Interrupt Autovector                       */
		 46, /* 31: Level 7 Interrupt Autovector                       */
		 38, /* 32: TRAP #0                                            */
		 38, /* 33: TRAP #1                                            */
		 38, /* 34: TRAP #2                                            */
		 38, /* 35: TRAP #3                                            */
		 38, /* 36: TRAP #4                                            */
		 38, /* 37: TRAP #5                                            */
		 38, /* 38: TRAP #6                                            */
		 38, /* 39: TRAP #7                                            */
		 38, /* 40: TRAP #8                                            */
		 38, /* 41: TRAP #9                                            */
		 38, /* 42: TRAP #10                                           */
		 38, /* 43: TRAP #11                                           */
		 38, /* 44: TRAP #12                                           */
		 38, /* 45: TRAP #13                                           */
		 38, /* 46: TRAP #14                                           */
		 38, /* 47: TRAP #15                                           */
		  4, /* 48: FP Branch or Set on Unknown Condition (unemulated) */
		  4, /* 49: FP Inexact Result                     (unemulated) */
		  4, /* 50: FP Divide by Zero                     (unemulated) */
		  4, /* 51: FP Underflow                          (unemulated) */
		  4, /* 52: FP Operand Error                      (unemulated) */
		  4, /* 53: FP Overflow                           (unemulated) */
		  4, /* 54: FP Signaling NAN                      (unemulated) */
		  4, /* 55: FP Unimplemented Data Type            (unemulated) */
		  4, /* 56: MMU Configuration Error               (unemulated) */
		  4, /* 57: MMU Illegal Operation Error           (unemulated) */
		  4, /* 58: MMU Access Level Violation Error      (unemulated) */
		  4, /* 59: RESERVED                                           */
		  4, /* 60: RESERVED                                           */
		  4, /* 61: RESERVED                                           */
		  4, /* 62: RESERVED                                           */
		  4, /* 63: RESERVED                                           */
		     /* 64-255: User Defined                                   */
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4
	},
	{ /* 020 */
		  4, /*  0: Reset - Initial Stack Pointer                      */
		  4, /*  1: Reset - Initial Program Counter                    */
		 50, /*  2: Bus Error                             (unemulated) */
		 50, /*  3: Address Error                         (unemulated) */
		 20, /*  4: Illegal Instruction                                */
		 38, /*  5: Divide by Zero                                     */
		 40, /*  6: CHK                                                */
		 20, /*  7: TRAPV                                              */
		 34, /*  8: Privilege Violation                                */
		 25, /*  9: Trace                                              */
		 20, /* 10: 1010                                               */
		 20, /* 11: 1111                                               */
		  4, /* 12: RESERVED                                           */
		  4, /* 13: Coprocessor Protocol Violation        (unemulated) */
		  4, /* 14: Format Error                                       */
		 30, /* 15: Uninitialized Interrupt                            */
		  4, /* 16: RESERVED                                           */
		  4, /* 17: RESERVED                                           */
		  4, /* 18: RESERVED                                           */
		  4, /* 19: RESERVED                                           */
		  4, /* 20: RESERVED                                           */
		  4, /* 21: RESERVED                                           */
		  4, /* 22: RESERVED                                           */
		  4, /* 23: RESERVED                                           */
		 30, /* 24: Spurious Interrupt                                 */
		 30, /* 25: Level 1 Interrupt Autovector                       */
		 30, /* 26: Level 2 Interrupt Autovector                       */
		 30, /* 27: Level 3 Interrupt Autovector                       */
		 30, /* 28: Level 4 Interrupt Autovector                       */
		 30, /* 29: Level 5 Interrupt Autovector                       */
		 30, /* 30: Level 6 Interrupt Autovector                       */
		 30, /* 31: Level 7 Interrupt Autovector                       */
		 20, /* 32: TRAP #0                                            */
		 20, /* 33: TRAP #1                                            */
		 20, /* 34: TRAP #2                                            */
		 20, /* 35: TRAP #3                                            */
		 20, /* 36: TRAP #4                                            */
		 20, /* 37: TRAP #5                                            */
		 20, /* 38: TRAP #6                                            */
		 20, /* 39: TRAP #7                                            */
		 20, /* 40: TRAP #8                                            */
		 20, /* 41: TRAP #9                                            */
		 20, /* 42: TRAP #10                                           */
		 20, /* 43: TRAP #11                                           */
		 20, /* 44: TRAP #12                                           */
		 20, /* 45: TRAP #13                                           */
		 20, /* 46: TRAP #14                                           */
		 20, /* 47: TRAP #15                                           */
		  4, /* 48: FP Branch or Set on Unknown Condition (unemulated) */
		  4, /* 49: FP Inexact Result                     (unemulated) */
		  4, /* 50: FP Divide by Zero                     (unemulated) */
		  4, /* 51: FP Underflow                          (unemulated) */
		  4, /* 52: FP Operand Error                      (unemulated) */
		  4, /* 53: FP Overflow                           (unemulated) */
		  4, /* 54: FP Signaling NAN                      (unemulated) */
		  4, /* 55: FP Unimplemented Data Type            (unemulated) */
		  4, /* 56: MMU Configuration Error               (unemulated) */
		  4, /* 57: MMU Illegal Operation Error           (unemulated) */
		  4, /* 58: MMU Access Level Violation Error      (unemulated) */
		  4, /* 59: RESERVED                                           */
		  4, /* 60: RESERVED                                           */
		  4, /* 61: RESERVED                                           */
		  4, /* 62: RESERVED                                           */
		  4, /* 63: RESERVED                                           */
		     /* 64-255: User Defined                                   */
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4
	},
	{ /* 030 - not correct */
		  4, /*  0: Reset - Initial Stack Pointer                      */
		  4, /*  1: Reset - Initial Program Counter                    */
		 50, /*  2: Bus Error                             (unemulated) */
		 50, /*  3: Address Error                         (unemulated) */
		 20, /*  4: Illegal Instruction                                */
		 38, /*  5: Divide by Zero                                     */
		 40, /*  6: CHK                                                */
		 20, /*  7: TRAPV                                              */
		 34, /*  8: Privilege Violation                                */
		 25, /*  9: Trace                                              */
		 20, /* 10: 1010                                               */
		 20, /* 11: 1111                                               */
		  4, /* 12: RESERVED                                           */
		  4, /* 13: Coprocessor Protocol Violation        (unemulated) */
		  4, /* 14: Format Error                                       */
		 30, /* 15: Uninitialized Interrupt                            */
		  4, /* 16: RESERVED                                           */
		  4, /* 17: RESERVED                                           */
		  4, /* 18: RESERVED                                           */
		  4, /* 19: RESERVED                                           */
		  4, /* 20: RESERVED                                           */
		  4, /* 21: RESERVED                                           */
		  4, /* 22: RESERVED                                           */
		  4, /* 23: RESERVED                                           */
		 30, /* 24: Spurious Interrupt                                 */
		 30, /* 25: Level 1 Interrupt Autovector                       */
		 30, /* 26: Level 2 Interrupt Autovector                       */
		 30, /* 27: Level 3 Interrupt Autovector                       */
		 30, /* 28: Level 4 Interrupt Autovector                       */
		 30, /* 29: Level 5 Interrupt Autovector                       */
		 30, /* 30: Level 6 Interrupt Autovector                       */
		 30, /* 31: Level 7 Interrupt Autovector                       */
		 20, /* 32: TRAP #0                                            */
		 20, /* 33: TRAP #1                                            */
		 20, /* 34: TRAP #2                                            */
		 20, /* 35: TRAP #3                                            */
		 20, /* 36: TRAP #4                                            */
		 20, /* 37: TRAP #5                                            */
		 20, /* 38: TRAP #6                                            */
		 20, /* 39: TRAP #7                                            */
		 20, /* 40: TRAP #8                                            */
		 20, /* 41: TRAP #9                                            */
		 20, /* 42: TRAP #10                                           */
		 20, /* 43: TRAP #11                                           */
		 20, /* 44: TRAP #12                                           */
		 20, /* 45: TRAP #13                                           */
		 20, /* 46: TRAP #14                                           */
		 20, /* 47: TRAP #15                                           */
		  4, /* 48: FP Branch or Set on Unknown Condition (unemulated) */
		  4, /* 49: FP Inexact Result                     (unemulated) */
		  4, /* 50: FP Divide by Zero                     (unemulated) */
		  4, /* 51: FP Underflow                          (unemulated) */
		  4, /* 52: FP Operand Error                      (unemulated) */
		  4, /* 53: FP Overflow                           (unemulated) */
		  4, /* 54: FP Signaling NAN                      (unemulated) */
		  4, /* 55: FP Unimplemented Data Type            (unemulated) */
		  4, /* 56: MMU Configuration Error               (unemulated) */
		  4, /* 57: MMU Illegal Operation Error           (unemulated) */
		  4, /* 58: MMU Access Level Violation Error      (unemulated) */
		  4, /* 59: RESERVED                                           */
		  4, /* 60: RESERVED                                           */
		  4, /* 61: RESERVED                                           */
		  4, /* 62: RESERVED                                           */
		  4, /* 63: RESERVED                                           */
		     /* 64-255: User Defined                                   */
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4
	},
	{ /* 040 */ /* TODO: these values are not correct */
		  4, /*  0: Reset - Initial Stack Pointer                      */
		  4, /*  1: Reset - Initial Program Counter                    */
		 50, /*  2: Bus Error                             (unemulated) */
		 50, /*  3: Address Error                         (unemulated) */
		 20, /*  4: Illegal Instruction                                */
		 38, /*  5: Divide by Zero                                     */
		 40, /*  6: CHK                                                */
		 20, /*  7: TRAPV                                              */
		 34, /*  8: Privilege Violation                                */
		 25, /*  9: Trace                                              */
		 20, /* 10: 1010                                               */
		 20, /* 11: 1111                                               */
		  4, /* 12: RESERVED                                           */
		  4, /* 13: Coprocessor Protocol Violation        (unemulated) */
		  4, /* 14: Format Error                                       */
		 30, /* 15: Uninitialized Interrupt                            */
		  4, /* 16: RESERVED                                           */
		  4, /* 17: RESERVED                                           */
		  4, /* 18: RESERVED                                           */
		  4, /* 19: RESERVED                                           */
		  4, /* 20: RESERVED                                           */
		  4, /* 21: RESERVED                                           */
		  4, /* 22: RESERVED                                           */
		  4, /* 23: RESERVED                                           */
		 30, /* 24: Spurious Interrupt                                 */
		 30, /* 25: Level 1 Interrupt Autovector                       */
		 30, /* 26: Level 2 Interrupt Autovector                       */
		 30, /* 27: Level 3 Interrupt Autovector                       */
		 30, /* 28: Level 4 Interrupt Autovector                       */
		 30, /* 29: Level 5 Interrupt Autovector                       */
		 30, /* 30: Level 6 Interrupt Autovector                       */
		 30, /* 31: Level 7 Interrupt Autovector                       */
		 20, /* 32: TRAP #0                                            */
		 20, /* 33: TRAP #1                                            */
		 20, /* 34: TRAP #2                                            */
		 20, /* 35: TRAP #3                                            */
		 20, /* 36: TRAP #4                                            */
		 20, /* 37: TRAP #5                                            */
		 20, /* 38: TRAP #6                                            */
		 20, /* 39: TRAP #7                                            */
		 20, /* 40: TRAP #8                                            */
		 20, /* 41: TRAP #9                                            */
		 20, /* 42: TRAP #10                                           */
		 20, /* 43: TRAP #11                                           */
		 20, /* 44: TRAP #12                                           */
		 20, /* 45: TRAP #13                                           */
		 20, /* 46: TRAP #14                                           */
		 20, /* 47: TRAP #15                                           */
		  4, /* 48: FP Branch or Set on Unknown Condition (unemulated) */
		  4, /* 49: FP Inexact Result                     (unemulated) */
		  4, /* 50: FP Divide by Zero                     (unemulated) */
		  4, /* 51: FP Underflow                          (unemulated) */
		  4, /* 52: FP Operand Error                      (unemulated) */
		  4, /* 53: FP Overflow                           (unemulated) */
		  4, /* 54: FP Signaling NAN                      (unemulated) */
		  4, /* 55: FP Unimplemented Data Type            (unemulated) */
		  4, /* 56: MMU Configuration Error               (unemulated) */
		  4, /* 57: MMU Illegal Operation Error           (unemulated) */
		  4, /* 58: MMU Access Level Violation Error      (unemulated) */
		  4, /* 59: RESERVED                                           */
		  4, /* 60: RESERVED                                           */
		  4, /* 61: RESERVED                                           */
		  4, /* 62: RESERVED                                           */
		  4, /* 63: RESERVED                                           */
		     /* 64-255: User Defined                                   */
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,
		  4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4,4
	}
};

const uint8 m68ki_ea_idx_cycle_table[64] =
{
	 0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,  0,
	 0, /* ..01.000 no memory indirect, base NULL             */
	 5, /* ..01..01 memory indirect,    base NULL, outer NULL */
	 7, /* ..01..10 memory indirect,    base NULL, outer 16   */
	 7, /* ..01..11 memory indirect,    base NULL, outer 32   */
	 0,  5,  7,  7,  0,  5,  7,  7,  0,  5,  7,  7,
	 2, /* ..10.000 no memory indirect, base 16               */
	 7, /* ..10..01 memory indirect,    base 16,   outer NULL */
	 9, /* ..10..10 memory indirect,    base 16,   outer 16   */
	 9, /* ..10..11 memory indirect,    base 16,   outer 32   */
	 0,  7,  9,  9,  0,  7,  9,  9,  0,  7,  9,  9,
	 6, /* ..11.000 no memory indirect, base 32               */
	11, /* ..11..01 memory indirect,    base 32,   outer NULL */
	13, /* ..11..10 memory indirect,    base 32,   outer 16   */
	13, /* ..11..11 memory indirect,    base 32,   outer 32   */
	 0, 11, 13, 13,  0, 11, 13, 13,  0, 11, 13, 13
};



/* ======================================================================== */
/* =============================== CALLBACKS ============================== */
/* ======================================================================== */

/* Default callbacks used if the callback hasn't been set yet, or if the
 * callback is set to NULL
 */

/* Interrupt acknowledge */
static int default_int_ack_callback_data;
static int default_int_ack_callback(int int_level)
{
	default_int_ack_callback_data = int_level;
	CPU_INT_LEVEL = 0;
	return M68K_INT_ACK_AUTOVECTOR;
}

/* Breakpoint acknowledge */
static unsigned int default_bkpt_ack_callback_data;
static void default_bkpt_ack_callback(unsigned int data)
{
	default_bkpt_ack_callback_data = data;
}

/* Called when a reset instruction is executed */
static void default_reset_instr_callback(void)
{
}

/* Called when a cmpi.l #v, dn instruction is executed */
static void default_cmpild_instr_callback(unsigned int val, int reg)
{
	(void)val;
	(void)reg;
}

/* Called when a rte instruction is executed */
static void default_rte_instr_callback(void)
{
}

/* Called when a tas instruction is executed */
static int default_tas_instr_callback(void)
{
	return 1; /* allow writeback */
}

/* Called when an illegal instruction is encountered */
static int default_illg_instr_callback(int opcode)
{
	(void)opcode;
	return 0; /* not handled : exception will occur */
}

/* Called when the program counter changed by a large value */
static unsigned int default_pc_changed_callback_data;
static void default_pc_changed_callback(unsigned int new_pc)
{
	default_pc_changed_callback_data = new_pc;
}

/* Called every time there's bus activity (read/write to/from memory */
static unsigned int default_set_fc_callback_data;
static void default_set_fc_callback(unsigned int new_fc)
{
	default_set_fc_callback_data = new_fc;
}

/* Called every instruction cycle prior to execution */
static void default_instr_hook_callback(unsigned int pc)
{
	(void)pc;
}


#if M68K_EMULATE_ADDRESS_ERROR
	#include <setjmp.h>
	#ifdef _BSD_SETJMP_H
	sigjmp_buf m68ki_aerr_trap;
	#else
	jmp_buf m68ki_aerr_trap;
	#endif
#endif /* M68K_EMULATE_ADDRESS_ERROR */

/* ======================================================================== */
/* ================================= API ================================== */
/* ======================================================================== */

/* Access the internals of the CPU */
unsigned int m68k_get_reg(void* context, m68k_register_t regnum)
{
	m68ki_cpu_core* cpu = context != NULL ?(m68ki_cpu_core*)context : &m68ki_cpu;

	switch(regnum)
	{
		case M68K_REG_D0:	return cpu->dar[0];
		case M68K_REG_D1:	return cpu->dar[1];
		case M68K_REG_D2:	return cpu->dar[2];
		case M68K_REG_D3:	return cpu->dar[3];
		case M68K_REG_D4:	return cpu->dar[4];
		case M68K_REG_D5:	return cpu->dar[5];
		case M68K_REG_D6:	return cpu->dar[6];
		case M68K_REG_D7:	return cpu->dar[7];
		case M68K_REG_A0:	return cpu->dar[8];
		case M68K_REG_A1:	return cpu->dar[9];
		case M68K_REG_A2:	return cpu->dar[10];
		case M68K_REG_A3:	return cpu->dar[11];
		case M68K_REG_A4:	return cpu->dar[12];
		case M68K_REG_A5:	return cpu->dar[13];
		case M68K_REG_A6:	return cpu->dar[14];
		case M68K_REG_A7:	return cpu->dar[15];
		case M68K_REG_PC:	return MASK_OUT_ABOVE_32(cpu->pc);
		case M68K_REG_SR:	return	cpu->t1_flag						|
									cpu->t0_flag						|
									(cpu->s_flag << 11)					|
									(cpu->m_flag << 11)					|
									cpu->int_mask						|
									((cpu->x_flag & XFLAG_SET) >> 4)	|
									((cpu->n_flag & NFLAG_SET) >> 4)	|
									((!cpu->not_z_flag) << 2)			|
									((cpu->v_flag & VFLAG_SET) >> 6)	|
									((cpu->c_flag & CFLAG_SET) >> 8);
		case M68K_REG_SP:	return cpu->dar[15];
		case M68K_REG_USP:	return cpu->s_flag ? cpu->sp[0] : cpu->dar[15];
		case M68K_REG_ISP:	return cpu->s_flag && !cpu->m_flag ? cpu->dar[15] : cpu->sp[4];
		case M68K_REG_MSP:	return cpu->s_flag && cpu->m_flag ? cpu->dar[15] : cpu->sp[6];
		case M68K_REG_SFC:	return cpu->sfc;
		case M68K_REG_DFC:	return cpu->dfc;
		case M68K_REG_VBR:	return cpu->vbr;
		case M68K_REG_CACR:	return cpu->cacr;
		case M68K_REG_CAAR:	return cpu->caar;
		case M68K_REG_PREF_ADDR:	return cpu->pref_addr;
		case M68K_REG_PREF_DATA:	return cpu->pref_data;
		case M68K_REG_PPC:	return MASK_OUT_ABOVE_32(cpu->ppc);
		case M68K_REG_IR:	return cpu->ir;
		case M68K_REG_CPU_TYPE:
			switch(cpu->cpu_type)
			{
				case CPU_TYPE_000:		return (unsigned int)M68K_CPU_TYPE_68000;
				case CPU_TYPE_010:		return (unsigned int)M68K_CPU_TYPE_68010;
				case CPU_TYPE_EC020:	return (unsigned int)M68K_CPU_TYPE_68EC020;
				case CPU_TYPE_020:		return (unsigned int)M68K_CPU_TYPE_68020;
				case CPU_TYPE_040:		return (unsigned int)M68K_CPU_TYPE_68040;
			}
			return M68K_CPU_TYPE_INVALID;
		default:			return 0;
	}
	return 0;
}

void m68k_set_reg(m68k_register_t regnum, unsigned int value)
{
	switch(regnum)
	{
		case M68K_REG_D0:	REG_D[0] = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_D1:	REG_D[1] = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_D2:	REG_D[2] = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_D3:	REG_D[3] = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_D4:	REG_D[4] = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_D5:	REG_D[5] = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_D6:	REG_D[6] = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_D7:	REG_D[7] = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_A0:	REG_A[0] = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_A1:	REG_A[1] = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_A2:	REG_A[2] = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_A3:	REG_A[3] = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_A4:	REG_A[4] = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_A5:	REG_A[5] = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_A6:	REG_A[6] = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_A7:	REG_A[7] = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_PC:	m68ki_jump(MASK_OUT_ABOVE_32(value)); return;
		case M68K_REG_SR:	m68ki_set_sr_noint_nosp(value); return;
		case M68K_REG_SP:	REG_SP = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_USP:	if(FLAG_S)
								REG_USP = MASK_OUT_ABOVE_32(value);
							else
								REG_SP = MASK_OUT_ABOVE_32(value);
							return;
		case M68K_REG_ISP:	if(FLAG_S && !FLAG_M)
								REG_SP = MASK_OUT_ABOVE_32(value);
							else
								REG_ISP = MASK_OUT_ABOVE_32(value);
							return;
		case M68K_REG_MSP:	if(FLAG_S && FLAG_M)
								REG_SP = MASK_OUT_ABOVE_32(value);
							else
								REG_MSP = MASK_OUT_ABOVE_32(value);
							return;
		case M68K_REG_VBR:	REG_VBR = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_SFC:	REG_SFC = value & 7; return;
		case M68K_REG_DFC:	REG_DFC = value & 7; return;
		case M68K_REG_CACR:	REG_CACR = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_CAAR:	REG_CAAR = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_PPC:	REG_PPC = MASK_OUT_ABOVE_32(value); return;
		case M68K_REG_IR:	REG_IR = MASK_OUT_ABOVE_16(value); return;
		case M68K_REG_CPU_TYPE: m68k_set_cpu_type(value); return;
		default:			return;
	}
}

/* Set the callbacks */
void m68k_set_int_ack_callback(int  (*callback)(int int_level))
{
	CALLBACK_INT_ACK = callback ? callback : default_int_ack_callback;
}

void m68k_set_bkpt_ack_callback(void  (*callback)(unsigned int data))
{
	CALLBACK_BKPT_ACK = callback ? callback : default_bkpt_ack_callback;
}

void m68k_set_reset_instr_callback(void  (*callback)(void))
{
	CALLBACK_RESET_INSTR = callback ? callback : default_reset_instr_callback;
}

void m68k_set_cmpild_instr_callback(void  (*callback)(unsigned int, int))
{
	CALLBACK_CMPILD_INSTR = callback ? callback : default_cmpild_instr_callback;
}

void m68k_set_rte_instr_callback(void  (*callback)(void))
{
	CALLBACK_RTE_INSTR = callback ? callback : default_rte_instr_callback;
}

void m68k_set_tas_instr_callback(int  (*callback)(void))
{
	CALLBACK_TAS_INSTR = callback ? callback : default_tas_instr_callback;
}

void m68k_set_illg_instr_callback(int  (*callback)(int))
{
	CALLBACK_ILLG_INSTR = callback ? callback : default_illg_instr_callback;
}

void m68k_set_pc_changed_callback(void  (*callback)(unsigned int new_pc))
{
	CALLBACK_PC_CHANGED = callback ? callback : default_pc_changed_callback;
}

void m68k_set_fc_callback(void  (*callback)(unsigned int new_fc))
{
	CALLBACK_SET_FC = callback ? callback : default_set_fc_callback;
}

void m68k_set_instr_hook_callback(void  (*callback)(unsigned int pc))
{
	CALLBACK_INSTR_HOOK = callback ? callback : default_instr_hook_callback;
}

/* Set the CPU type. */
void m68k_set_cpu_type(unsigned int cpu_type)
{
#ifdef ESP_PLATFORM
	tab5_dispatch_cache_reset();
#if PX68K_TAB5_X68P4_PREDECODE
	x68p4_pre138_invalidate_all_internal();
#endif
#endif
	switch(cpu_type)
	{
		case M68K_CPU_TYPE_68000:
			CPU_TYPE         = CPU_TYPE_000;
			CPU_ADDRESS_MASK = 0x00ffffff;
			CPU_SR_MASK      = 0xa71f; /* T1 -- S  -- -- I2 I1 I0 -- -- -- X  N  Z  V  C  */
			CYC_INSTRUCTION  = m68ki_cycles[0];
			CYC_EXCEPTION    = m68ki_exception_cycle_table[0];
			CYC_BCC_NOTAKE_B = -2;
			CYC_BCC_NOTAKE_W = 2;
			CYC_DBCC_F_NOEXP = -2;
			CYC_DBCC_F_EXP   = 2;
			CYC_SCC_R_TRUE   = 2;
			CYC_MOVEM_W      = 2;
			CYC_MOVEM_L      = 3;
			CYC_SHIFT        = 1;
			CYC_RESET        = 132;
			HAS_PMMU	 = 0;
			return;
		case M68K_CPU_TYPE_SCC68070:
			m68k_set_cpu_type(M68K_CPU_TYPE_68010);
			CPU_ADDRESS_MASK = 0xffffffff;
			CPU_TYPE         = CPU_TYPE_SCC070;
			return;
		case M68K_CPU_TYPE_68010:
			CPU_TYPE         = CPU_TYPE_010;
			CPU_ADDRESS_MASK = 0x00ffffff;
			CPU_SR_MASK      = 0xa71f; /* T1 -- S  -- -- I2 I1 I0 -- -- -- X  N  Z  V  C  */
			CYC_INSTRUCTION  = m68ki_cycles[1];
			CYC_EXCEPTION    = m68ki_exception_cycle_table[1];
			CYC_BCC_NOTAKE_B = -4;
			CYC_BCC_NOTAKE_W = 0;
			CYC_DBCC_F_NOEXP = 0;
			CYC_DBCC_F_EXP   = 6;
			CYC_SCC_R_TRUE   = 0;
			CYC_MOVEM_W      = 2;
			CYC_MOVEM_L      = 3;
			CYC_SHIFT        = 1;
			CYC_RESET        = 130;
			HAS_PMMU	 = 0;
			return;
		case M68K_CPU_TYPE_68EC020:
			CPU_TYPE         = CPU_TYPE_EC020;
			CPU_ADDRESS_MASK = 0x00ffffff;
			CPU_SR_MASK      = 0xf71f; /* T1 T0 S  M  -- I2 I1 I0 -- -- -- X  N  Z  V  C  */
			CYC_INSTRUCTION  = m68ki_cycles[2];
			CYC_EXCEPTION    = m68ki_exception_cycle_table[2];
			CYC_BCC_NOTAKE_B = -2;
			CYC_BCC_NOTAKE_W = 0;
			CYC_DBCC_F_NOEXP = 0;
			CYC_DBCC_F_EXP   = 4;
			CYC_SCC_R_TRUE   = 0;
			CYC_MOVEM_W      = 2;
			CYC_MOVEM_L      = 2;
			CYC_SHIFT        = 0;
			CYC_RESET        = 518;
			HAS_PMMU	 = 0;
			return;
		case M68K_CPU_TYPE_68020:
			CPU_TYPE         = CPU_TYPE_020;
			CPU_ADDRESS_MASK = 0xffffffff;
			CPU_SR_MASK      = 0xf71f; /* T1 T0 S  M  -- I2 I1 I0 -- -- -- X  N  Z  V  C  */
			CYC_INSTRUCTION  = m68ki_cycles[2];
			CYC_EXCEPTION    = m68ki_exception_cycle_table[2];
			CYC_BCC_NOTAKE_B = -2;
			CYC_BCC_NOTAKE_W = 0;
			CYC_DBCC_F_NOEXP = 0;
			CYC_DBCC_F_EXP   = 4;
			CYC_SCC_R_TRUE   = 0;
			CYC_MOVEM_W      = 2;
			CYC_MOVEM_L      = 2;
			CYC_SHIFT        = 0;
			CYC_RESET        = 518;
			HAS_PMMU	 = 0;
			return;
		case M68K_CPU_TYPE_68030:
			CPU_TYPE         = CPU_TYPE_030;
			CPU_ADDRESS_MASK = 0xffffffff;
			CPU_SR_MASK      = 0xf71f; /* T1 T0 S  M  -- I2 I1 I0 -- -- -- X  N  Z  V  C  */
			CYC_INSTRUCTION  = m68ki_cycles[3];
			CYC_EXCEPTION    = m68ki_exception_cycle_table[3];
			CYC_BCC_NOTAKE_B = -2;
			CYC_BCC_NOTAKE_W = 0;
			CYC_DBCC_F_NOEXP = 0;
			CYC_DBCC_F_EXP   = 4;
			CYC_SCC_R_TRUE   = 0;
			CYC_MOVEM_W      = 2;
			CYC_MOVEM_L      = 2;
			CYC_SHIFT        = 0;
			CYC_RESET        = 518;
			HAS_PMMU	       = 1;
			return;
		case M68K_CPU_TYPE_68EC030:
			CPU_TYPE         = CPU_TYPE_EC030;
			CPU_ADDRESS_MASK = 0xffffffff;
			CPU_SR_MASK          = 0xf71f; /* T1 T0 S  M  -- I2 I1 I0 -- -- -- X  N  Z  V  C  */
			CYC_INSTRUCTION  = m68ki_cycles[3];
			CYC_EXCEPTION    = m68ki_exception_cycle_table[3];
			CYC_BCC_NOTAKE_B = -2;
			CYC_BCC_NOTAKE_W = 0;
			CYC_DBCC_F_NOEXP = 0;
			CYC_DBCC_F_EXP   = 4;
			CYC_SCC_R_TRUE   = 0;
			CYC_MOVEM_W      = 2;
			CYC_MOVEM_L      = 2;
			CYC_SHIFT        = 0;
			CYC_RESET        = 518;
			HAS_PMMU	       = 0;		/* EC030 lacks the PMMU and is effectively a die-shrink 68020 */
			return;
		case M68K_CPU_TYPE_68040:		/* TODO: these values are not correct */
			CPU_TYPE         = CPU_TYPE_040;
			CPU_ADDRESS_MASK = 0xffffffff;
			CPU_SR_MASK      = 0xf71f; /* T1 T0 S  M  -- I2 I1 I0 -- -- -- X  N  Z  V  C  */
			CYC_INSTRUCTION  = m68ki_cycles[4];
			CYC_EXCEPTION    = m68ki_exception_cycle_table[4];
			CYC_BCC_NOTAKE_B = -2;
			CYC_BCC_NOTAKE_W = 0;
			CYC_DBCC_F_NOEXP = 0;
			CYC_DBCC_F_EXP   = 4;
			CYC_SCC_R_TRUE   = 0;
			CYC_MOVEM_W      = 2;
			CYC_MOVEM_L      = 2;
			CYC_SHIFT        = 0;
			CYC_RESET        = 518;
			HAS_PMMU	 = 1;
			return;
		case M68K_CPU_TYPE_68EC040: /* Just a 68040 without pmmu apparently... */
			CPU_TYPE         = CPU_TYPE_EC040;
			CPU_ADDRESS_MASK = 0xffffffff;
			CPU_SR_MASK      = 0xf71f; /* T1 T0 S  M  -- I2 I1 I0 -- -- -- X  N  Z  V  C  */
			CYC_INSTRUCTION  = m68ki_cycles[4];
			CYC_EXCEPTION    = m68ki_exception_cycle_table[4];
			CYC_BCC_NOTAKE_B = -2;
			CYC_BCC_NOTAKE_W = 0;
			CYC_DBCC_F_NOEXP = 0;
			CYC_DBCC_F_EXP   = 4;
			CYC_SCC_R_TRUE   = 0;
			CYC_MOVEM_W      = 2;
			CYC_MOVEM_L      = 2;
			CYC_SHIFT        = 0;
			CYC_RESET        = 518;
			HAS_PMMU	 = 0;
			return;
		case M68K_CPU_TYPE_68LC040:
			CPU_TYPE         = CPU_TYPE_LC040;
			m68ki_cpu.sr_mask          = 0xf71f; /* T1 T0 S  M  -- I2 I1 I0 -- -- -- X  N  Z  V  C  */
			m68ki_cpu.cyc_instruction  = m68ki_cycles[4];
			m68ki_cpu.cyc_exception    = m68ki_exception_cycle_table[4];
			m68ki_cpu.cyc_bcc_notake_b = -2;
			m68ki_cpu.cyc_bcc_notake_w = 0;
			m68ki_cpu.cyc_dbcc_f_noexp = 0;
			m68ki_cpu.cyc_dbcc_f_exp   = 4;
			m68ki_cpu.cyc_scc_r_true   = 0;
			m68ki_cpu.cyc_movem_w      = 2;
			m68ki_cpu.cyc_movem_l      = 2;
			m68ki_cpu.cyc_shift        = 0;
			m68ki_cpu.cyc_reset        = 518;
			HAS_PMMU	       = 1;
			return;
	}
}

/* R140P3: R57E65 wait-time slack prefetch retired with wall-clock pacing.
 * Instruction fetch/predecode remains authoritative. */

/* Execute some instructions until we use up num_cycles clock cycles */
/* ASG: removed per-instruction interrupt checks */
#ifdef ESP_PLATFORM
int IRAM_ATTR __attribute__((hot, optimize("O3"))) m68k_execute(int num_cycles)
#else
int m68k_execute(int num_cycles)
#endif
{
	/* eat up any reset cycles */
	if (RESET_CYCLES) {
	    int rc = RESET_CYCLES;
	    RESET_CYCLES = 0;
	    num_cycles -= rc;
	    if (num_cycles <= 0)
		return rc;
	}

	/* Set our pool of clock cycles available */
	SET_CYCLES(num_cycles);
	m68ki_initial_cycles = num_cycles;

	/* See if interrupts came in */
	m68ki_check_interrupts();

	/* Make sure we're not stopped */
	if(!CPU_STOPPED)
	{
		/* Return point if we had an address error */
		m68ki_set_address_error_trap(); /* auto-disable (see m68kcpu.h) */


		/* R57E62 production: MEM/IPL roots are TCM-pinned and stable for one
		 * m68k_execute() slice.  Hoist them out of the per-instruction fetch. */
#ifdef ESP_PLATFORM
		uint8_t * const tab5_mem_fetch_base = MEM;
		uint8_t * const tab5_ipl_fetch_base = IPL;
#if PX68K_TAB5_X68P4_PREDECODE
		/* Slice-local hot-page residency.  Self-modifying writes invalidate the
		 * directory version, so the per-instruction guard is one Internal-SRAM
		 * tag/version check rather than an opcode fetch/decode. */
		x68p4_pre138_op_t *tab5_pre138_page_ops = NULL;
		uint32_t tab5_pre138_page_tag = X68P4_PRE138_INVALID_TAG;
		uint32_t tab5_pre138_page_version = 0u;
		unsigned tab5_pre138_page_slot = 0u;
		uint32_t tab5_pre138_epoch = g_x68p4_pre139_epoch;
#endif
#endif

		/* Main loop.  Keep going until we run out of clock cycles */
		do
		{
			/* Set tracing accodring to T1. (T0 is done inside instruction) */
			m68ki_trace_t1(); /* auto-disable (see m68kcpu.h) */

			/* Set the address space for reads */
			m68ki_use_data_space(); /* auto-disable (see m68kcpu.h) */

#ifdef ESP_PLATFORM
			/* R57E138A: Trace134-137 runtime entry is retired.  The normal CPU
			 * path below is now the X68P4 predecoded-page executor. */

			/* Build 6.15h17R22: most MDX outer iterations are the four-op zero
			 * path.  Batch complete zero iterations first; fall through to the
			 * exact R20 handler block for non-zero entries / cycle-boundary
			 * tails, then to stock Musashi for anything else. */
			if (__builtin_expect((REG_PC & 0x00ffffffu) == TAB5_MDX619_PC_START, 0)) {
#if PX68K_TAB5_MDX622_ZERO_RUN_AB
				if (tab5_mdx622_zero_run_try()) {
					m68ki_exception_if_trace();
					continue;
				}
#endif
				if (tab5_mdx619_try()) {
					m68ki_exception_if_trace();
					continue;
				}
			}
#endif

			/* Call external hook to peek at CPU */
			m68ki_instr_hook(REG_PC); /* auto-disable (see m68kcpu.h) */

			/* Record previous program counter */
			REG_PPC = REG_PC;

			/* R140X5: resident page metadata is demand-decoded lazily.  After a PC word
			 * has executed once, the hot path uses its Internal-SRAM X68P4Op and
			 * never reloads that opcode from guest RAM/IPL.  First execution of a
			 * word demand-decodes only that word; old L1/L2/R123 remain retired. */
#ifdef ESP_PLATFORM
			{
				const uint32_t tab5_fetch_pc_raw = REG_PC;
				const uint32_t tab5_fetch_pc = ADDRESS_68K(tab5_fetch_pc_raw);
				uint16_t op;
				tab5_m68k_handler_t handler;
				uint32_t instr_cycles;
				uint32_t post_flags;
				const int tab5_r140m2_entry = tab5_r140m2_gate_pc(tab5_fetch_pc);

				m68ki_set_fc(FLAG_S | FUNCTION_CODE_USER_PROGRAM);
				m68ki_check_address_error(tab5_fetch_pc_raw, MODE_READ,
				                          FLAG_S | FUNCTION_CODE_USER_PROGRAM);
				REG_PC = tab5_fetch_pc_raw + 2u;

#if PX68K_TAB5_X68P4_PREDECODE
				/* R140P4S5C1: M2 already owns exact handler chains and cycles.  When
				 * the current 128-byte page is still the resident/coherent page, skip
				 * the otherwise redundant first-word resolve + X68P4Op unpack.  Cold,
				 * changed, evicted, trace, cycle-tail, and M2-specific reject cases all
				 * fall through to the original resolver and original M2 gate below. */
				if (__builtin_expect(tab5_r140m2_entry, 0) &&
				    __builtin_expect(CPU_TYPE == CPU_TYPE_000 && !FLAG_T1 && !FLAG_T0 &&
				                     GET_CYCLES() > 0, 1) &&
				    __builtin_expect(tab5_pre138_page_ops != NULL &&
				                     tab5_pre138_page_tag == (tab5_fetch_pc & ~X68P4_PRE138_PAGE_MASK) &&
				                     tab5_pre138_epoch == g_x68p4_pre139_epoch, 1) &&
				    tab5_r140m2_cert_ok(tab5_fetch_pc, tab5_pre138_page_tag,
				                         tab5_pre138_page_version, tab5_pre138_page_slot)) {
					const uint16_t m2op = tab5_r140p4s5c1_m2_first_op(tab5_fetch_pc);
					BusErrFlag = 0;
					REG_IR = (uint32_t)m2op;
					if (__builtin_expect(m2op != 0u &&
					                     m68k_tab5_mdxm2_exec(tab5_fetch_pc, m2op) != 0u, 1))
						goto tab5_r140p4s5c1_m2_done;
				}
				x68p4_pre138_op_t * const xop = x68p4_pre138_resolve(
				    tab5_fetch_pc, &tab5_pre138_page_tag, &tab5_pre138_page_version,
				    &tab5_pre138_page_slot, &tab5_pre138_page_ops, &tab5_pre138_epoch);
				if (__builtin_expect(xop != NULL, 1)) {
					uint32_t xsig = xop->sig_cycles;
					/* R140X5: bit31 is the predecode-valid marker.  The hot
					 * path was going to load sig_cycles anyway, so demand-decode adds
					 * only this strongly-not-taken bit test after first execution. */
					if (__builtin_expect((xsig & 0x80000000u) == 0u, 0)) {
						(void)x68p4_pre138_decode_word(
						    tab5_fetch_pc & ~X68P4_PRE138_PAGE_MASK,
						    tab5_pre138_page_slot,
						    (unsigned)((tab5_fetch_pc & X68P4_PRE138_PAGE_MASK) >> 1));
						xsig = xop->sig_cycles;
					}
					BusErrFlag = 0;
					op = (uint16_t)(xsig & 0xffffu);
					REG_IR = (uint32_t)op;
					handler = xop->sem;
					instr_cycles = (xsig >> 16) & 0xffu;
					post_flags = (xsig >> 24) & 0x7fu;
				} else
#endif
				{
					/* Exceptional execute-from-device/unbound-cache escape only.
					 * Ordinary RAM/IPL execution never enters this block once R138 is
					 * bound.  Keep it for correctness during bring-up, not as the CPU
					 * architecture. */
					uint16_t tab5_fetch_word;
					if (__builtin_expect(tab5_fetch_pc <= 0x00bffffeu, 1)) {
						__builtin_memcpy(&tab5_fetch_word, tab5_mem_fetch_base + tab5_fetch_pc,
						                 sizeof(tab5_fetch_word));
						BusErrFlag = 0;
					} else if (__builtin_expect(tab5_fetch_pc >= 0x00fc0000u &&
					                               tab5_fetch_pc <= 0x00fffffeu, 0)) {
						__builtin_memcpy(&tab5_fetch_word,
						                 tab5_ipl_fetch_base + (tab5_fetch_pc & 0x0003ffffu),
						                 sizeof(tab5_fetch_word));
						BusErrFlag = 0;
					} else {
						tab5_fetch_word = (uint16_t)cpu_readmem24_word(tab5_fetch_pc);
					}
					op = tab5_fetch_word;
					REG_IR = (uint32_t)op;
					handler = m68ki_instruction_jump_table[op];
					instr_cycles = CYC_INSTRUCTION[op];
					post_flags = tab5_post585_flags(op);
				}


				/* R140M2: authoritative cold/change fallback for the same exact blocks. */
				if (__builtin_expect(tab5_r140m2_entry, 0) &&
				    tab5_r140m2_cert_ok(tab5_fetch_pc, tab5_pre138_page_tag,
				                         tab5_pre138_page_version, tab5_pre138_page_slot) &&
				    m68k_tab5_mdxm2_exec(tab5_fetch_pc, op)) {
				}
				/* R140S2: one cold range compare is the only normal-path tax.
				 * Exact-PC checks and page-version certification stay inside the
				 * fixed SFXVI path; generic chaining/discovery is fully retired. */
				else if (__builtin_expect((tab5_fetch_pc & 0x00ffff00u) == 0x00368700u, 0) &&
				    (((tab5_fetch_pc == 0x00368760u) && (op == 0x3819u) &&
				      tab5_r140s2_sfx760_try(tab5_fetch_pc, tab5_pre138_page_tag, tab5_pre138_page_version, tab5_pre138_page_slot)) ||
				     (((tab5_fetch_pc == 0x00368784u) || (tab5_fetch_pc == 0x003687a4u) ||
				       (tab5_fetch_pc == 0x003687c4u)) && (op == 0xe483u) &&
				      tab5_r140s2_sfx784_try(tab5_fetch_pc, tab5_pre138_page_tag, tab5_pre138_page_version, tab5_pre138_page_slot)))) {
				}
				/* Build 5.98f: one rare metadata gate covers Bcc.B/W, DBcc and
				 * concrete register-only hot kernels.  Ordinary instructions keep the
				 * unchanged direct handler-call path. */
				else if (__builtin_expect(post_flags == 0u, 1)) {
					handler();
					USE_CYCLES(instr_cycles);
				} else {
					/* R57E130A: CORE_FAST dominates the measured dense mix.  Test it
					 * first inside the already-rare post_flags!=0 branch so hot arithmetic
					 * does not walk the four Bcc/DBcc tests before reaching its kernel. */
					if (__builtin_expect((post_flags & TAB5_POST598F_CORE_FAST) != 0u, 1)) {
						if (((op & 0xfff8u) == 0x48e0u || (op & 0xfff8u) == 0x4cd8u) &&
						    !tab5_movem598g9_try(op))
							handler();
						else if ((op & 0xfff8u) != 0x48e0u && (op & 0xfff8u) != 0x4cd8u)
							tab5_core598f_exec(op);
					} else if (post_flags & TAB5_POST598F_BNE_FAST) {
						if ((op & 0x00ffu) != 0u) {
							/* Preserve the proven 5.98d/5.98e byte-BNE hot path. */
							if (__builtin_expect(FLAG_Z != 0u, 1)) {
								REG_PC += MAKE_INT_8(op & 0x00ffu);
							} else
								USE_CYCLES(CYC_BCC_NOTAKE_B);
						} else {
							tab5_bcc598f_exec(op, FLAG_Z != 0u);
						}
					} else if (post_flags & TAB5_POST598F_BEQ_FAST) {
						if ((op & 0x00ffu) != 0u) {
							if (__builtin_expect(FLAG_Z == 0u, 1))
								REG_PC += MAKE_INT_8(op & 0x00ffu);
							else
								USE_CYCLES(CYC_BCC_NOTAKE_B);
						} else {
							tab5_bcc598f_exec(op, FLAG_Z == 0u);
						}
					} else if (post_flags & TAB5_POST598F_BCC_FAST) {
						tab5_bcc598f_exec(op, tab5_bcc598f_taken(op));
					} else if (post_flags & TAB5_POST585_DBF) {
						(void)tab5_dbcc598f_exec(op);
					} else {
						handler();
					}
					USE_CYCLES(instr_cycles);

					/* Build 6.13c: learn only actually-taken backward conditional
					 * edges.  Compilation happens after the current architectural
					 * iteration, so a failed/rejected translation cannot alter it. */

					/* Build 5.98g6: the current BNE cycles are now charged, so any
					 * future CLR/CMPA/BNE iterations may be bounded exactly by the
					 * remaining scheduler slice. */
					if (__builtin_expect((post_flags & TAB5_POST598F_BNE_FAST) &&
					                     ((op & 0x00ffu) == 0xfau) && FLAG_Z != 0u, 0))
						tab5_clear598g6_try_cmpa_bne(op);

					/* Build 6.15h17R17: accelerate the measured CMP.W (An)+ / BHI -4
					 * ordinary-RAM search only after the current BHI has completed. */
					if (__builtin_expect((post_flags & TAB5_POST598F_BCC_FAST) &&
					                     op == 0x62fcu &&
					                     ((REG_PC & 0x00ffffffu) < (REG_PPC & 0x00ffffffu)), 0))
						tab5_cmphi617_try(op);

					/* Build 5.98g11: the measured sparse-clear loop ends in the
					 * exact BPL.B -10 opcode $6AF6.  The helper verifies the full
					 * five-op RAM code sequence and that this branch was taken. */
					if (__builtin_expect((post_flags & TAB5_POST598F_BCC_FAST) && op == 0x6af6u, 0))
						tab5_sparse598g11_try_bpl(op);

					if (post_flags & TAB5_POST585_STREAM)
						tab5_stream576_try_postrun(op, instr_cycles);
					if (post_flags & TAB5_POST585_POLL)
						tab5_poll58_try(op);
					if ((post_flags & TAB5_POST585_DBF) && ((op & 0xfff8u) == 0x51c8u)) {
						/* DBF-only accelerators remain exactly where they were: after one
						 * architectural iteration has completed. */
						if ((REG_PC & 0x00ffffffu) == (REG_PPC & 0x00ffffffu))
							tab5_dbf73_try_self(op);
						else if (!tab5_dbf74_try_store_body(op))
							tab5_fill59_try_dbf(op);
					}

				}
				tab5_r140p4s5c1_m2_done:
				;
			}
#else
			m68ki_instruction_jump_table[REG_IR]();
			USE_CYCLES(CYC_INSTRUCTION[REG_IR]);
#endif

			/* Trace m68k_exception, if necessary */
			m68ki_exception_if_trace(); /* auto-disable (see m68kcpu.h) */
		} while(GET_CYCLES() > 0);

		/* set previous PC to current PC for the next entry into the loop */
		REG_PPC = REG_PC;
	}
	else
		SET_CYCLES(0);

	/* return how many clocks we used */
	return m68ki_initial_cycles - GET_CYCLES();
}


int m68k_cycles_run(void)
{
	return m68ki_initial_cycles - GET_CYCLES();
}

int m68k_cycles_remaining(void)
{
	return GET_CYCLES();
}

/* Change the timeslice */
void m68k_modify_timeslice(int cycles)
{
	m68ki_initial_cycles += cycles;
	ADD_CYCLES(cycles);
}


void m68k_end_timeslice(void)
{
	m68ki_initial_cycles = GET_CYCLES();
	SET_CYCLES(0);
}


/* ASG: rewrote so that the int_level is a mask of the IPL0/IPL1/IPL2 bits */
/* KS: Modified so that IPL* bits match with mask positions in the SR
 *     and cleaned out remenants of the interrupt controller.
 */
void m68k_set_irq(unsigned int int_level)
{
	uint old_level = CPU_INT_LEVEL;
	CPU_INT_LEVEL = int_level << 8;

	/* A transition from < 7 to 7 always interrupts (NMI) */
	/* Note: Level 7 can also level trigger like a normal IRQ */
	if(old_level != 0x0700 && CPU_INT_LEVEL == 0x0700)
		m68ki_cpu.nmi_pending = 1;
}

void m68k_set_virq(unsigned int level, unsigned int active)
{
	uint state = m68ki_cpu.virq_state;
	uint blevel;

	if(active)
		state |= 1 << level;
	else
		state &= ~(1 << level);
	m68ki_cpu.virq_state = state;

	for(blevel = 7; blevel > 0; blevel--)
		if(state & (1 << blevel))
			break;
	m68k_set_irq(blevel);
}

unsigned int m68k_get_virq(unsigned int level)
{
	return (m68ki_cpu.virq_state & (1 << level)) ? 1 : 0;
}



void m68k_init(void)
{
	static uint emulation_initialized = 0;

	/* The first call to this function initializes the opcode handler jump table */
	if(!emulation_initialized)
		{
		m68ki_build_opcode_table();
#ifdef ESP_PLATFORM
		s_tab5_stream581_pie_enabled = GVRAM_P4StreamSelfcheck();
		printf("PX68K_STREAM581: Build 5.89a P4 PIE/XespV repeat backend self-check %s; repeat=%s copy=SCALAR\n",
		       s_tab5_stream581_pie_enabled ? "PASS" : "FAIL",
		       s_tab5_stream581_pie_enabled ? "PIE-128" : "SCALAR-FALLBACK");
		printf("PX68K_POST585: fused dispatch-metadata post-op gate ACTIVE; normal instruction path tests one flag word, stream/poll/DBF recognizers unchanged\n");
		printf("PX68K_BRANCH598F: Bcc.B/Bcc.W + DBcc inline via existing dispatch metadata gate; exact cycles/extension fetch retained\n");
		printf("PX68K_CPU598G10: g9 hot paths + MOVE.L (An)+,(Am)+ / BTST #imm,Dn / CLR.L (An)+ / EXT.L Dn inline armed; generated-handler semantics retained\n");
		printf("PX68K_CPUHOT_R57E47: NW16 wide inline pack RETIRED; NW14 execute loop restored; NW15-measured FLASH hot handlers use IRAM12 placement only\n");
#if PX68K_TAB5_MDX622_ZERO_RUN_AB
#else
		printf("PX68K_CPU615H29: R28 proven IRAM11 + profile-driven IRAM64 total handlers; ZERO-RUN=OFF, device exact paths retained\n");
#endif
		printf("PX68K_LOOP598G11: exact CLR.W(A0)+ x2 / SUBQ.W D0 / ADDQ #4,A0 / BPL -10 ordinary-RAM batch armed; scheduler boundary + final exit preserved\n");
		printf("X68P4_PRE138: legacy dispatch sentinels only L1=%u L2=%uB R123=%uB; normal execute path does not access them\n",
		       (unsigned)TAB5_M68K_DISPATCH_CACHE_SIZE, (unsigned)sizeof(s_tab5_dispatch_l2),
		       (unsigned)sizeof(g_tab5_exec123_cache));
        printf("PX68K_EXEC_R57E128B: legacy fast semantic helpers retained only behind predecoded-op bridge; R123 runtime RETIRED\n");
        printf("PX68K_EXEC_R57E130A: hot-family semantic kernels retained; fused PC/opcode lookup RETIRED\n");
        printf("PX68K_EXEC_R140J2: X5 demand-decode/coldmeta retained + deadline-driven Machine Kernel ACTIVE; fixed 200/800 horizon retired; exact Musashi semantics/cycles retained\n");
        printf("PX68K_EXEC_P12R1: STANDARD12 EXACT EXECUTOR ACTIVE; exact 10/12 CPU-peripheral conversion; MK1 deadlines retained; runtime discovery ZERO\n");
		printf("PX68K_EXEC_R140P4MK1: Machine Kernel COMMON0 ACTIVE; exact MFP IRQ-deadline shadow + early-return window + RTC IRQ15 gate; J2 deadlines/cycles retained; runtime discovery ZERO\n");
        printf("PX68K_EXEC_R140S2: STATIC SFXVI superinstructions ACTIVE; generic warm chain RETIRED; runtime trace/JIT/discovery ZERO; exact J2 CORE_FAST path restored\n");
        printf("PX68K_EXEC_R140M2: MDX exact-page fused basic blocks ACTIVE; M1B profiler RETIRED; direct static Musashi handler chains; runtime discovery ZERO\n");
        printf("PX68K_EXEC_R140P4S5C1: M2 EARLY STATIC ENTRY ACTIVE; resident-page exact blocks bypass redundant first-word resolve; cold/change fallback exact; runtime discovery ZERO\n");
#endif
		emulation_initialized = 1;
	}

	m68k_set_int_ack_callback(NULL);
	m68k_set_bkpt_ack_callback(NULL);
	m68k_set_reset_instr_callback(NULL);
	m68k_set_cmpild_instr_callback(NULL);
	m68k_set_rte_instr_callback(NULL);
	m68k_set_tas_instr_callback(NULL);
	m68k_set_illg_instr_callback(NULL);
	m68k_set_pc_changed_callback(NULL);
	m68k_set_fc_callback(NULL);
	m68k_set_instr_hook_callback(NULL);
}


/* Pulse the RESET line on the CPU */
void m68k_pulse_reset(void)
{
	/* Disable the PMMU on reset */
	m68ki_cpu.pmmu_enabled = 0;

	/* Clear all stop levels and eat up all remaining cycles */
	CPU_STOPPED = 0;
	SET_CYCLES(0);

	CPU_RUN_MODE = RUN_MODE_BERR_AERR_RESET;
	CPU_INSTR_MODE = INSTRUCTION_YES;

	/* Turn off tracing */
	FLAG_T1 = FLAG_T0 = 0;
	m68ki_clear_trace();
	/* Interrupt mask to level 7 */
	FLAG_INT_MASK = 0x0700;
	CPU_INT_LEVEL = 0;
	m68ki_cpu.virq_state = 0;
	/* Reset VBR */
	REG_VBR = 0;
	/* Go to supervisor mode */
	m68ki_set_sm_flag(SFLAG_SET | MFLAG_CLEAR);

	/* Invalidate the prefetch queue */
#if M68K_EMULATE_PREFETCH
	/* Set to arbitrary number since our first fetch is from 0 */
	CPU_PREF_ADDR = 0x1000;
#endif /* M68K_EMULATE_PREFETCH */

	/* Read the initial stack pointer and program counter */
	m68ki_jump(0);
	REG_SP = m68ki_read_imm_32();
	REG_PC = m68ki_read_imm_32();
	m68ki_jump(REG_PC);

	CPU_RUN_MODE = RUN_MODE_NORMAL;

	RESET_CYCLES = CYC_EXCEPTION[EXCEPTION_RESET];
}

/* Pulse the HALT line on the CPU */
void m68k_pulse_halt(void)
{
	CPU_STOPPED |= STOP_LEVEL_HALT;
}

/* Get and set the current CPU context */
/* This is to allow for multiple CPUs */
unsigned int m68k_context_size()
{
	return sizeof(m68ki_cpu_core);
}

unsigned int m68k_get_context(void* dst)
{
	if(dst) *(m68ki_cpu_core*)dst = m68ki_cpu;
	return sizeof(m68ki_cpu_core);
}

void m68k_set_context(void* src)
{
	if(src) m68ki_cpu = *(m68ki_cpu_core*)src;
}

/* ======================================================================== */
/* ============================== END OF FILE ============================= */
/* ======================================================================== */
