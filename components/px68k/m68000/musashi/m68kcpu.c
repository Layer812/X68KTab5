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

#ifndef PX68K_TAB5_PERF_PROFILE
#define PX68K_TAB5_PERF_PROFILE 0
#endif
#if PX68K_TAB5_PERF_PROFILE
#define TAB5_PROF(...) do { __VA_ARGS__; } while (0)
#else
#define TAB5_PROF(...) do { } while (0)
#endif

/* Build 5.64: PX68K's current Musashi integration has no call site for
 * m68k_pulse_bus_error(); its own BusErrFlag/BusErrHandling bookkeeping is
 * not bridged to Musashi's longjmp bus-error mechanism.  Therefore the
 * per-instruction 64-byte D/A rollback snapshot and per-slice bus-error
 * setjmp were pure preparation for an unreachable exception path.
 *
 * Keep this as an explicit compile-time switch so a future port that wires
 * m68k_pulse_bus_error() back in can restore the upstream behavior simply by
 * setting the value to 0.  Address-error emulation remains untouched. */
/* Intent: The original rollback preparation is unnecessary for the Tab5 bus-error path; skip only dead preparation work while retaining address-error behavior.  Layer8 Aug/17/2026 */
#ifndef PX68K_TAB5_SKIP_DEAD_BERR_ROLLBACK
#define PX68K_TAB5_SKIP_DEAD_BERR_ROLLBACK 1
#endif

/* Build 5.56a: P4 XespV copies the 64-byte D0-D7/A0-A7 bus-error snapshot
 * with three 128-bit loads + three 128-bit stores.  The arrays are explicitly
 * from the native Musashi layout.  The global CPU object is 16-byte aligned;
 * dar/dar_save are both +4 mod 16, so the helper uses a 12-byte scalar head,
 * three aligned 128-bit moves, and one 4-byte scalar tail. */
extern void tab5_xespv_copy64(void *dst, const void *src);
_Static_assert((offsetof(m68ki_cpu_core, dar) & 15u) == 4u, "unexpected Musashi dar layout");
_Static_assert((offsetof(m68ki_cpu_core, dar_save) & 15u) == 4u, "unexpected Musashi dar_save layout");

/* Runtime guard for the experimental P4 PIE/XespV copy.  Build 5.56 aborted on
 * a mismatch before Musashi started.  5.56a keeps the self-check but falls back
 * to the proven scalar snapshot so one experimental instruction sequence can
 * never prevent Human68k from booting. */
static DRAM_ATTR int s_tab5_xespv_snapshot_enabled = 0;
static DRAM_ATTR __attribute__((aligned(16))) uint8_t s_tab5_xespv_probe_src[80];
static DRAM_ATTR __attribute__((aligned(16))) uint8_t s_tab5_xespv_probe_dst[80];

static inline __attribute__((always_inline)) void tab5_scalar_copy64(uint *dst, const uint *src)
{
    dst[0]=src[0]; dst[1]=src[1]; dst[2]=src[2]; dst[3]=src[3];
    dst[4]=src[4]; dst[5]=src[5]; dst[6]=src[6]; dst[7]=src[7];
    dst[8]=src[8]; dst[9]=src[9]; dst[10]=src[10]; dst[11]=src[11];
    dst[12]=src[12]; dst[13]=src[13]; dst[14]=src[14]; dst[15]=src[15];
}

static void tab5_xespv_snapshot_selfcheck(void)
{
    uint32_t saved_dar[16], saved_copy[16], ref[16];
    unsigned i;
    int dram_ok = 0, tcm_ok = 0;
    s_tab5_xespv_snapshot_enabled = 0;

    /* RC: one deterministic exactness check replaces development-time A/B timing. */
    for (i = 0; i < 64; ++i) s_tab5_xespv_probe_src[4+i] = (uint8_t)(0x31u + i * 7u);
    memset(s_tab5_xespv_probe_dst, 0, sizeof(s_tab5_xespv_probe_dst));
    tab5_xespv_copy64(s_tab5_xespv_probe_dst + 4, s_tab5_xespv_probe_src + 4);
    dram_ok = (memcmp(s_tab5_xespv_probe_src + 4, s_tab5_xespv_probe_dst + 4, 64) == 0);

    if ((((uintptr_t)&m68ki_cpu) & 15u) != 0u) {
        printf("PX68K_XESPV599RC1: exact self-check DRAM=%s TCM=SKIP backend=SCALAR\n",
               dram_ok ? "PASS" : "FAIL");
        return;
    }

    memcpy(saved_dar, m68ki_cpu.dar, sizeof(saved_dar));
    memcpy(saved_copy, m68ki_cpu.dar_save, sizeof(saved_copy));
    for (i = 0; i < 16; ++i) m68ki_cpu.dar[i] = 0x13579bdfu ^ (0x01020304u * i);
    memcpy(ref, m68ki_cpu.dar, sizeof(ref));
    memset(m68ki_cpu.dar_save, 0, sizeof(m68ki_cpu.dar_save));
    tab5_xespv_copy64(m68ki_cpu.dar_save, m68ki_cpu.dar);
    tcm_ok = (memcmp(ref, m68ki_cpu.dar_save, sizeof(ref)) == 0);
    if (dram_ok && tcm_ok) s_tab5_xespv_snapshot_enabled = 1;

    memcpy(m68ki_cpu.dar, saved_dar, sizeof(saved_dar));
    memcpy(m68ki_cpu.dar_save, saved_copy, sizeof(saved_copy));
    printf("PX68K_XESPV599RC1: exact self-check DRAM=%s TCM=%s backend=%s\n",
           dram_ok ? "PASS" : "FAIL", tcm_ok ? "PASS" : "FAIL",
           s_tab5_xespv_snapshot_enabled ? "XespV" : "SCALAR");
}

/*
 * Build 5.20: sample-only 68000 opcode profiler.
 *
 * The full 64K counter array intentionally lives with the PX68K component BSS
 * (PSRAM in the Tab5 linker layout).  It is touched only during the existing
 * one-frame-per-600 performance sample, so normal emulation has just one
 * highly-predictable branch per instruction and no counter traffic.
 *
 * This is diagnostic scaffolding for a later selective IRAM fast path: rather
 * than moving Musashi's entire 256KB jump table into scarce internal SRAM, we
 * first identify the actual hot 68000 opcodes used by real X68000 software.
 */
static uint32_t s_tab5_opcode_counts[0x10000];
static uint32_t s_tab5_opcode_total = 0;
static DRAM_ATTR int s_tab5_opcode_profile_enabled = 0;

/* profile-safe code peek */
static inline __attribute__((always_inline)) uint16_t tab5_poll58_fetch16(uint32_t a);

/* Build 5.98g6: observation-only map of 68000 instruction shapes that may
 * benefit from P4 PIE/SIMD batching.  This runs only during the existing sparse
 * opcode-profile window, so normal emulation pays zero permanent cost.  It
 * intentionally does NOT alter guest state and does not claim that every hit
 * is safe to vectorize; the counters tell us where a later exact fast path is
 * worth implementing. */
typedef struct {
    uint32_t movem_total;
    uint32_t movem_w;
    uint32_t movem_l;
    uint32_t movem_r2m;
    uint32_t movem_m2r;
    uint32_t movem_regs;
    uint32_t movem_bytes;
    uint32_t movem_r2m_bytes;
    uint32_t movem_m2r_bytes;
    uint32_t movem_max_regs;
    uint32_t movem_predec;
    uint32_t movem_postinc;
    uint32_t store_w_post;
    uint32_t store_l_post;
    uint32_t copy_w_post;
    uint32_t copy_l_post;
    uint32_t clr_mem;
    uint32_t or_mem;
    uint32_t and_mem;
    uint32_t eor_mem;
    uint32_t mem_shift;
} tab5_piecpu598g4b_obs_t;
static DRAM_ATTR tab5_piecpu598g4b_obs_t s_tab5_piecpu598g4b;

static inline __attribute__((always_inline))
void tab5_piecpu598g4b_observe(uint16_t op)
{
    tab5_piecpu598g4b_obs_t * const o = &s_tab5_piecpu598g4b;
    const unsigned mode = (op >> 3) & 7u;

    if ((op & 0xfb80u) == 0x4880u) {
        const uint32_t epc = REG_PC & 0x00ffffffu;
        ++o->movem_total;
        if (op & 0x0040u) ++o->movem_l; else ++o->movem_w;
        if (op & 0x0400u) ++o->movem_m2r; else ++o->movem_r2m;
        if (mode == 4u) ++o->movem_predec;
        if (mode == 3u) ++o->movem_postinc;
        /* The register mask is the immediate extension word.  Peek it only
         * from ordinary RAM/IPL so observation never touches device space. */
        if (epc <= 0x00bffffeu || (epc >= 0x00fc0000u && epc <= 0x00fffffeu)) {
            const uint16_t mask = tab5_poll58_fetch16(epc);
            const uint32_t regs = (uint32_t)__builtin_popcount((unsigned)mask);
            const uint32_t bytes = regs * ((op & 0x0040u) ? 4u : 2u);
            o->movem_regs += regs;
            o->movem_bytes += bytes;
            if (op & 0x0400u) o->movem_m2r_bytes += bytes;
            else o->movem_r2m_bytes += bytes;
            if (regs > o->movem_max_regs) o->movem_max_regs = regs;
        }
        return;
    }

    if ((op & 0xf1f8u) == 0x30c0u) { ++o->store_w_post; return; }
    if ((op & 0xf1f8u) == 0x20c0u) { ++o->store_l_post; return; }
    if ((op & 0xf1f8u) == 0x30d8u) { ++o->copy_w_post; return; }
    if ((op & 0xf1f8u) == 0x20d8u) { ++o->copy_l_post; return; }

    if ((op & 0xff00u) == 0x4200u && mode >= 2u) { ++o->clr_mem; return; }
    if (((op >> 6) & 7u) >= 4u && ((op >> 6) & 7u) <= 6u && mode >= 2u) {
        switch ((op >> 12) & 15u) {
        case 0x8u: ++o->or_mem; return;
        case 0xbu: ++o->eor_mem; return;
        case 0xcu: ++o->and_mem; return;
        default: break;
        }
    }
    if ((op & 0xf000u) == 0xe000u && (op & 0x00c0u) == 0x00c0u && mode >= 2u)
        ++o->mem_shift;
}

static void tab5_piecpu598g4b_reset(void)
{
    memset(&s_tab5_piecpu598g4b, 0, sizeof(s_tab5_piecpu598g4b));
}


/* Build 5.65b: observation-only study of consecutive MOVE.W Dn,(An)+ runs.
 * Unlike 5.65/5.65a this never reads future guest instructions and never
 * changes PC/registers/cycles/memory. It runs only inside the sparse opcode
 * profiling window, using instructions Musashi has already fetched. */
typedef struct {
    uint32_t total_candidates;
    uint32_t runs_ge2;
    uint32_t cur_len;
    uint32_t cur_start_pc;
    uint32_t prev_pc;
    uint16_t prev_op;
    uint32_t max_len;
    uint32_t max_start_pc;
    uint32_t max_end_pc;
    uint16_t max_op;
} tab5_movew65b_obs_t;
static DRAM_ATTR tab5_movew65b_obs_t s_tab5_movew65b;


/* Build 5.70: observation remains diagnostic-only.  The runtime accelerator
 * below no longer depends on learning a block after it has already executed:
 * it validates the live RAM instruction stream at the current PC immediately
 * before any instructions are elided. */
static inline __attribute__((always_inline))
void tab5_movew570_finish_observed_run(tab5_movew65b_obs_t *o)
{
    if (o->cur_len >= 2u) o->runs_ge2++;
}

static inline __attribute__((always_inline))
void tab5_movew65b_observe(uint16_t op, uint32_t pc)
{
    tab5_movew65b_obs_t * const o = &s_tab5_movew65b;
    pc &= 0x00ffffffu;
    if ((op & 0xf1f8u) == 0x30c0u) { /* MOVE.W Dn,(An)+ */
        o->total_candidates++;
        if (o->cur_len && op == o->prev_op &&
            pc == ((o->prev_pc + 2u) & 0x00ffffffu)) {
            o->cur_len++;
        } else {
            if (o->cur_len) tab5_movew570_finish_observed_run(o);
            o->cur_len = 1u;
            o->cur_start_pc = pc;
        }
        o->prev_op = op;
        o->prev_pc = pc;
        if (o->cur_len > o->max_len) {
            o->max_len = o->cur_len;
            o->max_op = op;
            o->max_start_pc = o->cur_start_pc;
            o->max_end_pc = pc;
        }
    } else {
        if (o->cur_len) tab5_movew570_finish_observed_run(o);
        o->cur_len = 0u;
        o->prev_op = 0u;
        o->prev_pc = pc;
    }
}

static void tab5_movew65b_reset(void)
{
    memset(&s_tab5_movew65b, 0, sizeof(s_tab5_movew65b));
}

static void tab5_movew65b_dump(void)
{
    tab5_movew65b_obs_t * const o = &s_tab5_movew65b;
    const uint32_t runs = o->runs_ge2 + (o->cur_len >= 2u ? 1u : 0u);
    printf("PX68K_FILL65B_OBS: logical candidates=%lu runs>=2=%lu maxrun=%lu op=$%04X pc=$%06lX-$%06lX; live-run batching may account for elided opcodes\n",
           (unsigned long)o->total_candidates,
           (unsigned long)runs,
           (unsigned long)o->max_len,
           (unsigned)o->max_op,
           (unsigned long)o->max_start_pc,
           (unsigned long)o->max_end_pc);
}


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
#ifndef TAB5_M68K_DISPATCH_CACHE_BITS
#define TAB5_M68K_DISPATCH_CACHE_BITS 8u
#endif
#define TAB5_M68K_DISPATCH_CACHE_SIZE (1u << TAB5_M68K_DISPATCH_CACHE_BITS)
#define TAB5_M68K_DISPATCH_CACHE_MASK (TAB5_M68K_DISPATCH_CACHE_SIZE - 1u)
/* Build 5.98g9: TCM L1 + internal-DRAM L2 avoids PSRAM metadata misses. */
#define TAB5_M68K_DISPATCH_L2_BITS 9u
#define TAB5_M68K_DISPATCH_L2_SIZE (1u << TAB5_M68K_DISPATCH_L2_BITS)
#define TAB5_M68K_DISPATCH_L2_MASK (TAB5_M68K_DISPATCH_L2_SIZE - 1u)

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

#ifdef TCM_DRAM_ATTR
static TCM_DRAM_ATTR tab5_m68k_dispatch_entry_t s_tab5_dispatch_cache[TAB5_M68K_DISPATCH_CACHE_SIZE];
static TCM_DRAM_ATTR uint32_t s_tab5_dispatch_prof_enabled = 0;
static TCM_DRAM_ATTR uint32_t s_tab5_dispatch_hits = 0;
static TCM_DRAM_ATTR uint32_t s_tab5_dispatch_misses = 0;
#else
static DRAM_ATTR tab5_m68k_dispatch_entry_t s_tab5_dispatch_cache[TAB5_M68K_DISPATCH_CACHE_SIZE];
static DRAM_ATTR uint32_t s_tab5_dispatch_prof_enabled = 0;
static DRAM_ATTR uint32_t s_tab5_dispatch_hits = 0;
static DRAM_ATTR uint32_t s_tab5_dispatch_misses = 0;
#endif

static DRAM_ATTR tab5_m68k_dispatch_entry_t s_tab5_dispatch_l2[TAB5_M68K_DISPATCH_L2_SIZE];
static DRAM_ATTR uint32_t s_tab5_dispatch_l2_hits = 0;

/* Build 5.59 poll accelerator diagnostics live in ordinary internal DRAM,
 * not scarce TCM. */
static DRAM_ATTR uint32_t s_tab5_poll58_diag_moveand = 0;
static DRAM_ATTR uint32_t s_tab5_poll58_diag_btst = 0;
static DRAM_ATTR uint32_t s_tab5_poll58_diag_skipped = 0;
static DRAM_ATTR uint32_t s_tab5_poll58_total_moveand = 0;
static DRAM_ATTR uint32_t s_tab5_poll58_total_btst = 0;
static DRAM_ATTR uint32_t s_tab5_poll59_diag_fdc = 0;
static DRAM_ATTR uint32_t s_tab5_poll59_total_fdc = 0;
static DRAM_ATTR uint64_t s_tab5_poll58_total_skipped = 0;
static DRAM_ATTR uint64_t s_tab5_poll59_total_fdc_skipped = 0;
static DRAM_ATTR uint32_t s_tab5_poll58_announce_mask = 0;
static DRAM_ATTR uint32_t s_tab5_poll58_observe_mask = 0;
static DRAM_ATTR uint32_t s_tab5_poll58_last_pc = 0;

/* Build 5.73: generic side-effect-free RAM polling + pure DBF self-loop. */
static DRAM_ATTR uint32_t s_tab5_poll73_diag_ram_moveand = 0;
static DRAM_ATTR uint32_t s_tab5_poll73_diag_ram_btst = 0;
static DRAM_ATTR uint32_t s_tab5_poll73_diag_ram_cmpi = 0;
static DRAM_ATTR uint32_t s_tab5_poll598f_diag_ram_tstw = 0;
static DRAM_ATTR uint32_t s_tab5_poll598g6_diag_ram_tstw_abs = 0;
static DRAM_ATTR uint32_t s_tab5_poll73_total_ram_moveand = 0;
static DRAM_ATTR uint32_t s_tab5_poll73_total_ram_btst = 0;
static DRAM_ATTR uint32_t s_tab5_poll73_total_ram_cmpi = 0;
static DRAM_ATTR uint32_t s_tab5_poll598f_total_ram_tstw = 0;
static DRAM_ATTR uint32_t s_tab5_poll598g6_total_ram_tstw_abs = 0;
static DRAM_ATTR uint32_t s_tab5_dbf73_diag_hits = 0;
static DRAM_ATTR uint32_t s_tab5_dbf73_diag_loops = 0;
static DRAM_ATTR uint32_t s_tab5_dbf73_diag_maxbatch = 0;
static DRAM_ATTR uint32_t s_tab5_dbf73_total_hits = 0;
static DRAM_ATTR uint64_t s_tab5_dbf73_total_loops = 0;
static DRAM_ATTR uint64_t s_tab5_dbf73_total_cycles = 0;
static DRAM_ATTR uint32_t s_tab5_dbf73_total_maxbatch = 0;
static DRAM_ATTR uint32_t s_tab5_dbf73_announced = 0;

/* Build 5.98f: sampled fast-path attribution.  Counters increment only while
 * the existing opcode-profile window is enabled, so production hot paths do
 * not pay permanent instrumentation overhead. */
static DRAM_ATTR uint32_t s_tab5_fast598f_bccw = 0;
static DRAM_ATTR uint32_t s_tab5_fast598f_dbcc = 0;
static DRAM_ATTR uint32_t s_tab5_fast598f_core = 0;
static DRAM_ATTR uint32_t s_tab5_fast598f_core_mem = 0;

/* Build 5.74: short repeated-store DBF body accelerator. */
static DRAM_ATTR uint32_t s_tab5_dbf74_diag_hits = 0;
static DRAM_ATTR uint32_t s_tab5_dbf74_diag_loops = 0;
static DRAM_ATTR uint32_t s_tab5_dbf74_diag_maxbody = 0;
static DRAM_ATTR uint32_t s_tab5_dbf74_diag_maxbatch = 0;
static DRAM_ATTR uint32_t s_tab5_dbf74_total_hits = 0;
static DRAM_ATTR uint64_t s_tab5_dbf74_total_loops = 0;
static DRAM_ATTR uint64_t s_tab5_dbf74_total_instr = 0;
static DRAM_ATTR uint64_t s_tab5_dbf74_total_bytes = 0;
static DRAM_ATTR uint64_t s_tab5_dbf74_total_cycles = 0;
static DRAM_ATTR uint32_t s_tab5_dbf74_total_maxbody = 0;
static DRAM_ATTR uint32_t s_tab5_dbf74_total_maxbatch = 0;
static DRAM_ATTR uint32_t s_tab5_dbf74_announced_mask = 0;

/* Build 5.79: generalized scheduler-bounded MOVE stream engine.
 * The first instruction in each chunk executes normally; only a verified
 * homogeneous suffix is elided.  The descriptor records operand FORM rather
 * than a fixed memory REGION.  In particular, (An)+ is one generic AREG
 * source and the runtime backend dispatches RAM vs GVRAM from the live EA.
 * Backends today: Dn->GVRAM, RAM (An)+->GVRAM, GVRAM (An)+->GVRAM, MOVE.W. */
static DRAM_ATTR uint32_t s_tab5_stream576_reg_hits = 0;
static DRAM_ATTR uint64_t s_tab5_stream576_reg_words = 0;
static DRAM_ATTR uint32_t s_tab5_stream576_ram_hits = 0;
static DRAM_ATTR uint64_t s_tab5_stream576_ram_words = 0;
static DRAM_ATTR uint32_t s_tab5_stream576_gv_hits = 0;
static DRAM_ATTR uint64_t s_tab5_stream576_gv_words = 0;
static DRAM_ATTR uint64_t s_tab5_stream576_cycles = 0;
static DRAM_ATTR uint32_t s_tab5_stream576_maxchunk = 0;
static DRAM_ATTR uint32_t s_tab5_stream576_same_next = 0;
static DRAM_ATTR uint32_t s_tab5_stream576_src_reject = 0;
static DRAM_ATTR uint32_t s_tab5_stream576_dst_reject = 0;
/* Build 5.78: distinguish architectural 32-bit A-register values from the
 * X68000's 24-bit external bus address.  This both accelerates safe aliases
 * and makes any true non-RAM/GVRAM source reject self-identifying. */
static DRAM_ATTR uint32_t s_tab5_stream578_alias_hits = 0;
static DRAM_ATTR uint64_t s_tab5_stream578_alias_words = 0;
static DRAM_ATTR uint32_t s_tab5_stream578_alias_ram_hits = 0;
static DRAM_ATTR uint32_t s_tab5_stream578_alias_gv_hits = 0;
static DRAM_ATTR uint32_t s_tab5_stream578_alias_ipl_hits = 0;
static DRAM_ATTR uint32_t s_tab5_stream578_ipl_hits = 0;
static DRAM_ATTR uint64_t s_tab5_stream578_ipl_words = 0;
static DRAM_ATTR uint32_t s_tab5_stream579_tv_hits = 0;
static DRAM_ATTR uint64_t s_tab5_stream579_tv_words = 0;
static DRAM_ATTR uint32_t s_tab5_stream579_alias_tv_hits = 0;
static DRAM_ATTR uint32_t s_tab5_stream578_rej_tvram = 0;
static DRAM_ATTR uint32_t s_tab5_stream578_rej_mmio = 0;
static DRAM_ATTR uint32_t s_tab5_stream578_rej_font = 0;
static DRAM_ATTR uint32_t s_tab5_stream578_rej_other = 0;
static DRAM_ATTR uint32_t s_tab5_stream578_dst_alias_hits = 0;
static DRAM_ATTR uint32_t s_tab5_stream576_announced_mask = 0;


/* Build 5.81: production P4 hybrid selected from the 5.80a live A/B.
 * DREG->GVRAM repeat streams keep the P4 PIE/XespV 128-bit backend when
 * eligible; native-word RAM/TVRAM/IPL copy streams stay on the scalar 5.79
 * backend because measured PIE copy cost was substantially higher.  No live
 * alternation or cycle sampling remains in the hot path. */
static DRAM_ATTR int s_tab5_stream581_pie_enabled = 0;
static DRAM_ATTR uint32_t s_tab5_stream581_repeat_pie_calls = 0;
static DRAM_ATTR uint64_t s_tab5_stream581_repeat_pie_words = 0;
static DRAM_ATTR uint64_t s_tab5_stream581_repeat_vec_words = 0;
static DRAM_ATTR uint32_t s_tab5_stream581_repeat_scalar_calls = 0;
static DRAM_ATTR uint64_t s_tab5_stream581_repeat_scalar_words = 0;
static DRAM_ATTR uint32_t s_tab5_stream581_pie_fallbacks = 0;

/* Build 5.59: exact zero-fill micro-loop accelerator diagnostics. */
static DRAM_ATTR uint32_t s_tab5_fill59_calls = 0;
static DRAM_ATTR uint32_t s_tab5_fill59_loops = 0;
static DRAM_ATTR uint32_t s_tab5_fill59_instr = 0;
static DRAM_ATTR uint64_t s_tab5_fill59_bytes = 0;
static DRAM_ATTR uint64_t s_tab5_fill59_cycles = 0;

/* Build 5.98g6: exact CLR.L (An)+ / CMPA.L Am,An / BNE.B back-edge
 * accelerator diagnostics. */
static DRAM_ATTR uint32_t s_tab5_clear598g6_calls = 0;
static DRAM_ATTR uint64_t s_tab5_clear598g6_loops = 0;
static DRAM_ATTR uint64_t s_tab5_clear598g6_instr = 0;
static DRAM_ATTR uint64_t s_tab5_clear598g6_bytes = 0;
static DRAM_ATTR uint64_t s_tab5_clear598g6_cycles = 0;
static DRAM_ATTR uint32_t s_tab5_clear598g6_maxbatch = 0;
static DRAM_ATTR uint32_t s_tab5_clear598g6_announced = 0;

/* Build 5.98g7: measured single-op handler elimination counters.  These two
 * families dominated the g6 profile but are not vector-friendly by themselves:
 * MOVE.B Dn,Dm is register-only, while MOVE.W Dn,(An)+ may touch any bus
 * region.  Execute their exact generated-Musashi bodies inline and keep the
 * existing STREAM hook after the word store so future homogeneous runs can
 * still graduate to the PIE backend. */
static DRAM_ATTR uint32_t s_tab5_fast598g7_moveb_rr = 0;
static DRAM_ATTR uint32_t s_tab5_fast598g7_movew_pi = 0;
static DRAM_ATTR uint32_t s_tab5_fast598g9_movel_pi = 0;
static DRAM_ATTR uint32_t s_tab5_fast598g9_clrw_pi = 0;
static DRAM_ATTR uint32_t s_tab5_fast598g9_addq_a = 0;
static DRAM_ATTR uint32_t s_tab5_fast598g9_movem_pd = 0;
static DRAM_ATTR uint32_t s_tab5_fast598g9_movem_pi = 0;
static DRAM_ATTR uint32_t s_tab5_fast598g9_movem_regs = 0;
static DRAM_ATTR uint32_t s_tab5_fast598g9_movem_bytes = 0;
/* Build 5.98g10: remaining measured single-op hot families. */
static DRAM_ATTR uint32_t s_tab5_fast598g10_movel_pipi = 0;
static DRAM_ATTR uint32_t s_tab5_fast598g10_btst_imm_d = 0;
static DRAM_ATTR uint32_t s_tab5_fast598g10_clrl_pi = 0;
static DRAM_ATTR uint32_t s_tab5_fast598g10_extl = 0;

/* Build 5.98g11: exact sparse-clear hot-loop batching.  The g10 profile
 * showed the five-instruction loop below consuming ~80% of one heavy sample:
 *   CLR.W (A0)+ ; CLR.W (A0)+ ; SUBQ.W #1,D0 ; ADDQ #4,A0 ; BPL.B loop
 * Only ordinary RAM writes are elided, within the current Musashi slice. */
static DRAM_ATTR uint32_t s_tab5_loop598g11_calls = 0;
static DRAM_ATTR uint64_t s_tab5_loop598g11_loops = 0;
static DRAM_ATTR uint64_t s_tab5_loop598g11_instr = 0;
static DRAM_ATTR uint64_t s_tab5_loop598g11_zero_bytes = 0;
static DRAM_ATTR uint64_t s_tab5_loop598g11_span_bytes = 0;
static DRAM_ATTR uint64_t s_tab5_loop598g11_cycles = 0;
static DRAM_ATTR uint32_t s_tab5_loop598g11_maxbatch = 0;
static DRAM_ATTR uint32_t s_tab5_loop598g11_announced = 0;


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
        TAB5_PROF(++s_tab5_fast598g9_movem_pd);
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
        TAB5_PROF(++s_tab5_fast598g9_movem_pi);
    } else {
        return 0;
    }

    TAB5_PROF(s_tab5_fast598g9_movem_regs += count);
    TAB5_PROF(s_tab5_fast598g9_movem_bytes += bytes);
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

static DRAM_ATTR uint32_t s_tab5_movew570_scans = 0;
static DRAM_ATTR uint32_t s_tab5_movew570_batches = 0;
static DRAM_ATTR uint64_t s_tab5_movew570_words = 0;
static DRAM_ATTR uint64_t s_tab5_movew570_cycles = 0;
static DRAM_ATTR uint32_t s_tab5_movew570_max_batch = 0;
static DRAM_ATTR uint32_t s_tab5_movew570_fallbacks = 0;

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

    TAB5_PROF(++s_tab5_movew570_scans);
    n = px68k_m68k_live_repeat_movew_ram(pc, a, value, op, max_n);
    if (n < 2u)
    {
        TAB5_PROF(++s_tab5_movew570_fallbacks);
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
    if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && n > 1u && s_tab5_opcode_profile_enabled, 0))
    {
        tab5_movew65b_obs_t * const o = &s_tab5_movew65b;
        const uint32_t extra = n - 1u;
        s_tab5_opcode_counts[op] += extra;
        s_tab5_opcode_total += extra;
        o->total_candidates += extra;
        if (o->cur_len && o->prev_op == op && o->prev_pc == pc)
        {
            o->cur_len += extra;
            o->prev_pc = pc + (extra << 1);
            if (o->cur_len > o->max_len)
            {
                o->max_len = o->cur_len;
                o->max_op = op;
                o->max_start_pc = o->cur_start_pc;
                o->max_end_pc = o->prev_pc;
            }
        }
    }
    if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && n > 1u && s_tab5_dispatch_prof_enabled, 0))
        s_tab5_dispatch_hits += n - 1u;

    TAB5_PROF(++s_tab5_movew570_batches);
    TAB5_PROF(s_tab5_movew570_words += n);
    TAB5_PROF(s_tab5_movew570_cycles += (uint64_t)n * move_cycles);
    TAB5_PROF(if (n > s_tab5_movew570_max_batch) s_tab5_movew570_max_batch = n);
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
            TAB5_PROF(++s_tab5_stream581_repeat_pie_calls);
            TAB5_PROF(s_tab5_stream581_repeat_pie_words += n);
            TAB5_PROF(s_tab5_stream581_repeat_vec_words += vec_words);
            return done;
        }
        TAB5_PROF(++s_tab5_stream581_pie_fallbacks);
    }

    TAB5_PROF(++s_tab5_stream581_repeat_scalar_calls);
    TAB5_PROF(s_tab5_stream581_repeat_scalar_words += n);
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
    if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && (s_tab5_opcode_profile_enabled || s_tab5_dispatch_prof_enabled), 0))
        return 0;
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
    TAB5_PROF(++s_tab5_stream576_same_next);

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
        TAB5_PROF(++s_tab5_stream576_dst_reject);
        return 0;
    }
    if (dst_arch & 0xff000000u)
        TAB5_PROF(++s_tab5_stream578_dst_alias_hits);
    {
        const uint32_t dst_words = (0x00d00000u - dst) >> 1;
        if (n > dst_words) n = dst_words;
        if (n < 2u) {
            TAB5_PROF(++s_tab5_stream576_dst_reject);
            return 0;
        }
    }

    if (d.src_kind == TAB5_STREAM576_SRC_DREG) {
        last_value = REG_D[d.src_reg] & 0xffffu;
        done = tab5_stream581_repeat_backend(dst, (uint16_t)last_value, n);
        if (done != n) {
            TAB5_PROF(++s_tab5_stream576_dst_reject);
            return 0;
        }
        TAB5_PROF(++s_tab5_stream576_reg_hits);
        TAB5_PROF(s_tab5_stream576_reg_words += n);
        if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && !(s_tab5_stream576_announced_mask & 1u), 0)) {
            s_tab5_stream576_announced_mask |= 1u;
            printf("PX68K_STREAM580: ACTIVE kind=DREG->GVRAM op=$%04X D%u->A%u first_chunk=%lu; scheduler boundary preserved\n",
                   (unsigned)op, d.src_reg, d.dst_reg, (unsigned long)n);
        }
    } else {
        const uint32_t src_arch = REG_A[d.src_reg];
        uint32_t src = src_arch & 0x00ffffffu;
        const int alias24 = (src_arch & 0xff000000u) != 0u;
        uint16_t last_word = 0;

        if (src & 1u) {
            TAB5_PROF(++s_tab5_stream576_src_reject);
            TAB5_PROF(++s_tab5_stream578_rej_other);
            return 0;
        }

        if (src < 0x00c00000u) {
            const uint32_t src_words = (0x00c00000u - src) >> 1;
            if (n > src_words) n = src_words;
            if (n < 2u) {
                TAB5_PROF(++s_tab5_stream576_src_reject);
                TAB5_PROF(++s_tab5_stream578_rej_other);
                return 0;
            }

            done = tab5_stream581_copy_backend(dst, MEM + src, n, &last_word);
            if (done != n) {
                TAB5_PROF(++s_tab5_stream576_dst_reject);
                return 0;
            }
            TAB5_PROF(++s_tab5_stream576_ram_hits);
            TAB5_PROF(s_tab5_stream576_ram_words += n);
            TAB5_PROF(if (alias24) ++s_tab5_stream578_alias_ram_hits);
            if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && !(s_tab5_stream576_announced_mask & 2u), 0)) {
                s_tab5_stream576_announced_mask |= 2u;
                printf("PX68K_STREAM580: ACTIVE kind=RAM->GVRAM op=$%04X A%u->A%u first_chunk=%lu; scheduler boundary preserved\n",
                       (unsigned)op, d.src_reg, d.dst_reg, (unsigned long)n);
            }
        } else if (src < 0x00e00000u) {
            const uint32_t src_words = (0x00e00000u - src) >> 1;
            if (n > src_words) n = src_words;
            if (n < 2u) {
                TAB5_PROF(++s_tab5_stream576_src_reject);
                TAB5_PROF(++s_tab5_stream578_rej_other);
                return 0;
            }

            done = GVRAM_CopyWordStream256(src, dst, n, &last_word);
            if (done != n) {
                TAB5_PROF(++s_tab5_stream576_src_reject);
                TAB5_PROF(++s_tab5_stream578_rej_other);
                return 0;
            }
            TAB5_PROF(++s_tab5_stream576_gv_hits);
            TAB5_PROF(s_tab5_stream576_gv_words += n);
            TAB5_PROF(if (alias24) ++s_tab5_stream578_alias_gv_hits);
            if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && !(s_tab5_stream576_announced_mask & 4u), 0)) {
                s_tab5_stream576_announced_mask |= 4u;
                printf("PX68K_STREAM580: ACTIVE kind=GVRAM->GVRAM op=$%04X A%u->A%u first_chunk=%lu; overlap-safe + scheduler boundary preserved\n",
                       (unsigned)op, d.src_reg, d.dst_reg, (unsigned long)n);
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
                TAB5_PROF(++s_tab5_stream576_src_reject);
                TAB5_PROF(++s_tab5_stream578_rej_tvram);
                return 0;
            }
            done = tab5_stream581_copy_backend(dst, TVRAM + (src & 0x0007ffffu), n, &last_word);
            if (done != n) {
                TAB5_PROF(++s_tab5_stream576_dst_reject);
                return 0;
            }
            TAB5_PROF(++s_tab5_stream579_tv_hits);
            TAB5_PROF(s_tab5_stream579_tv_words += n);
            TAB5_PROF(if (alias24) ++s_tab5_stream579_alias_tv_hits);
            if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && !(s_tab5_stream576_announced_mask & 32u), 0)) {
                s_tab5_stream576_announced_mask |= 32u;
                printf("PX68K_STREAM580: ACTIVE kind=TVRAM->GVRAM op=$%04X A%u->A%u first_chunk=%lu; side-effect-free native-word source\n",
                       (unsigned)op, d.src_reg, d.dst_reg, (unsigned long)n);
            }
        } else if (src >= 0x00fc0000u) {
            /* IPL is a plain 256 KiB ROM in the same host-native word-swapped
             * representation as MEM.  Reads are side-effect free, so this is
             * a safe stream backend too. */
            const uint32_t src_words = (0x01000000u - src) >> 1;
            if (n > src_words) n = src_words;
            if (n < 2u) {
                TAB5_PROF(++s_tab5_stream576_src_reject);
                TAB5_PROF(++s_tab5_stream578_rej_other);
                return 0;
            }
            done = tab5_stream581_copy_backend(dst, IPL + (src & 0x0003ffffu), n, &last_word);
            if (done != n) {
                TAB5_PROF(++s_tab5_stream576_dst_reject);
                return 0;
            }
            TAB5_PROF(++s_tab5_stream578_ipl_hits);
            TAB5_PROF(s_tab5_stream578_ipl_words += n);
            TAB5_PROF(if (alias24) ++s_tab5_stream578_alias_ipl_hits);
            if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && !(s_tab5_stream576_announced_mask & 8u), 0)) {
                s_tab5_stream576_announced_mask |= 8u;
                printf("PX68K_STREAM580: ACTIVE kind=IPL->GVRAM op=$%04X A%u->A%u first_chunk=%lu; ROM read side effects absent\n",
                       (unsigned)op, d.src_reg, d.dst_reg, (unsigned long)n);
            }
        } else {
            TAB5_PROF(
                ++s_tab5_stream576_src_reject;
                if (src < 0x00e80000u) ++s_tab5_stream578_rej_tvram;
                else if (src < 0x00f00000u) ++s_tab5_stream578_rej_mmio;
                else if (src < 0x00fc0000u) ++s_tab5_stream578_rej_font;
                else ++s_tab5_stream578_rej_other;
            );
            return 0;
        }

        if (alias24) {
            TAB5_PROF(++s_tab5_stream578_alias_hits);
            TAB5_PROF(s_tab5_stream578_alias_words += n);
            if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && !(s_tab5_stream576_announced_mask & 16u), 0)) {
                s_tab5_stream576_announced_mask |= 16u;
                printf("PX68K_STREAM580: 24-bit A-reg alias ACTIVE arch=$%08lX bus=$%06lX A%u; full A preserved, bus low24 used\n",
                       (unsigned long)src_arch, (unsigned long)src, d.src_reg);
            }
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

    TAB5_PROF(s_tab5_stream576_cycles += (uint64_t)n * move_cycles);
    TAB5_PROF(if (n > s_tab5_stream576_maxchunk) s_tab5_stream576_maxchunk = n);
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

void m68k_tab5_dispatch_profile_set(int enabled)
{
    enabled = enabled ? 1 : 0;
    if (enabled && !s_tab5_dispatch_prof_enabled) {
        s_tab5_dispatch_hits = 0;
        s_tab5_dispatch_l2_hits = 0;
        s_tab5_dispatch_misses = 0;
        s_tab5_poll58_diag_moveand = 0;
        s_tab5_poll58_diag_btst = 0;
        s_tab5_poll59_diag_fdc = 0;
        s_tab5_poll58_diag_skipped = 0;
        s_tab5_poll58_last_pc = 0;
        s_tab5_poll73_diag_ram_moveand = 0;
        s_tab5_poll73_diag_ram_btst = 0;
        s_tab5_poll73_diag_ram_cmpi = 0;
        s_tab5_poll598f_diag_ram_tstw = 0;
        s_tab5_poll598g6_diag_ram_tstw_abs = 0;
        s_tab5_fast598f_bccw = 0;
        s_tab5_fast598f_dbcc = 0;
        s_tab5_fast598f_core = 0;
        s_tab5_fast598f_core_mem = 0;
        s_tab5_fast598g7_moveb_rr = 0;
        s_tab5_fast598g7_movew_pi = 0;
        s_tab5_fast598g9_movel_pi = 0;
        s_tab5_fast598g9_clrw_pi = 0;
        s_tab5_fast598g9_addq_a = 0;
        s_tab5_fast598g9_movem_pd = 0;
        s_tab5_fast598g9_movem_pi = 0;
        s_tab5_fast598g9_movem_regs = 0;
        s_tab5_fast598g9_movem_bytes = 0;
        s_tab5_fast598g10_movel_pipi = 0;
        s_tab5_fast598g10_btst_imm_d = 0;
        s_tab5_fast598g10_clrl_pi = 0;
        s_tab5_fast598g10_extl = 0;
        s_tab5_dbf73_diag_hits = 0;
        s_tab5_dbf73_diag_loops = 0;
        s_tab5_dbf73_diag_maxbatch = 0;
        s_tab5_dbf74_diag_hits = 0;
        s_tab5_dbf74_diag_loops = 0;
        s_tab5_dbf74_diag_maxbody = 0;
        s_tab5_dbf74_diag_maxbatch = 0;
        s_tab5_dispatch_prof_enabled = 1;
    } else if (!enabled && s_tab5_dispatch_prof_enabled) {
        const uint32_t h = s_tab5_dispatch_hits;
        const uint32_t m = s_tab5_dispatch_misses;
        const uint32_t total = h + s_tab5_dispatch_l2_hits + m;
        s_tab5_dispatch_prof_enabled = 0;
        const uint32_t hit_x10 = total ? (h * 1000u) / total : 0u;
        printf("PX68K_P4CPU: DISPATCH L1=%u hit=%lu L2=%u hit=%lu backing=%lu L1rate=%lu.%lu%% tcm=%uB l2dram=%uB\n",
               (unsigned)TAB5_M68K_DISPATCH_CACHE_SIZE,
               (unsigned long)h,
               (unsigned)TAB5_M68K_DISPATCH_L2_SIZE,
               (unsigned long)s_tab5_dispatch_l2_hits,
               (unsigned long)m,
               (unsigned long)(hit_x10 / 10u),
               (unsigned long)(hit_x10 % 10u),
               (unsigned)sizeof(s_tab5_dispatch_cache),
               (unsigned)sizeof(s_tab5_dispatch_l2));
        printf("PX68K_POLL73: sample gpip(moveand/btst)=%lu/%lu fdc=%lu skipped=%lu cycles lastpc=$%06lX; "
               "total_gp=%lu/%lu total_fdc=%lu skipped_total=%llu fdc_skipped=%llu\n",
               (unsigned long)s_tab5_poll58_diag_moveand,
               (unsigned long)s_tab5_poll58_diag_btst,
               (unsigned long)s_tab5_poll59_diag_fdc,
               (unsigned long)s_tab5_poll58_diag_skipped,
               (unsigned long)(s_tab5_poll58_last_pc & 0x00ffffffu),
               (unsigned long)s_tab5_poll58_total_moveand,
               (unsigned long)s_tab5_poll58_total_btst,
               (unsigned long)s_tab5_poll59_total_fdc,
               (unsigned long long)s_tab5_poll58_total_skipped,
               (unsigned long long)s_tab5_poll59_total_fdc_skipped);
        printf("PX68K_POLL598G6: sample ram(moveand/btst/cmpi/tstw-an/tstw-abs)=%lu/%lu/%lu/%lu/%lu; total=%lu/%lu/%lu/%lu/%lu\n",
               (unsigned long)s_tab5_poll73_diag_ram_moveand,
               (unsigned long)s_tab5_poll73_diag_ram_btst,
               (unsigned long)s_tab5_poll73_diag_ram_cmpi,
               (unsigned long)s_tab5_poll598f_diag_ram_tstw,
               (unsigned long)s_tab5_poll598g6_diag_ram_tstw_abs,
               (unsigned long)s_tab5_poll73_total_ram_moveand,
               (unsigned long)s_tab5_poll73_total_ram_btst,
               (unsigned long)s_tab5_poll73_total_ram_cmpi,
               (unsigned long)s_tab5_poll598f_total_ram_tstw,
               (unsigned long)s_tab5_poll598g6_total_ram_tstw_abs);
        printf("PX68K_FAST598G9: sample bcc.w=%lu dbcc=%lu core=%lu core-mem=%lu moveb-rr=%lu movew-pi=%lu movel-pi=%lu clrw-pi=%lu addq-a=%lu MOVEM pd/pi=%lu/%lu regs=%lu bytes=%lu\n",
               (unsigned long)s_tab5_fast598f_bccw,
               (unsigned long)s_tab5_fast598f_dbcc,
               (unsigned long)s_tab5_fast598f_core,
               (unsigned long)s_tab5_fast598f_core_mem,
               (unsigned long)s_tab5_fast598g7_moveb_rr,
               (unsigned long)s_tab5_fast598g7_movew_pi,
               (unsigned long)s_tab5_fast598g9_movel_pi,
               (unsigned long)s_tab5_fast598g9_clrw_pi,
               (unsigned long)s_tab5_fast598g9_addq_a,
               (unsigned long)s_tab5_fast598g9_movem_pd,
               (unsigned long)s_tab5_fast598g9_movem_pi,
               (unsigned long)s_tab5_fast598g9_movem_regs,
               (unsigned long)s_tab5_fast598g9_movem_bytes);
        printf("PX68K_FAST598G10: sample movel-pipi=%lu btst-imm-d=%lu clrl-pi=%lu extl=%lu\n",
               (unsigned long)s_tab5_fast598g10_movel_pipi,
               (unsigned long)s_tab5_fast598g10_btst_imm_d,
               (unsigned long)s_tab5_fast598g10_clrl_pi,
               (unsigned long)s_tab5_fast598g10_extl);
        printf("PX68K_LOOP598G11: total calls=%lu loops=%llu instr-elided=%llu zero-bytes=%llu span-bytes=%llu cycles=%llu maxbatch=%lu\n",
               (unsigned long)s_tab5_loop598g11_calls,
               (unsigned long long)s_tab5_loop598g11_loops,
               (unsigned long long)s_tab5_loop598g11_instr,
               (unsigned long long)s_tab5_loop598g11_zero_bytes,
               (unsigned long long)s_tab5_loop598g11_span_bytes,
               (unsigned long long)s_tab5_loop598g11_cycles,
               (unsigned long)s_tab5_loop598g11_maxbatch);
        printf("PX68K_DBF73: sample hits=%lu loops=%lu maxbatch=%lu; total hits=%lu loops=%llu cycles=%llu maxbatch=%lu\n",
               (unsigned long)s_tab5_dbf73_diag_hits,
               (unsigned long)s_tab5_dbf73_diag_loops,
               (unsigned long)s_tab5_dbf73_diag_maxbatch,
               (unsigned long)s_tab5_dbf73_total_hits,
               (unsigned long long)s_tab5_dbf73_total_loops,
               (unsigned long long)s_tab5_dbf73_total_cycles,
               (unsigned long)s_tab5_dbf73_total_maxbatch);
        printf("PX68K_DBF74: store-body total hits=%lu loops=%llu instr=%llu bytes=%llu cycles=%llu maxbody=%lu maxbatch=%lu\n",
               (unsigned long)s_tab5_dbf74_total_hits,
               (unsigned long long)s_tab5_dbf74_total_loops,
               (unsigned long long)s_tab5_dbf74_total_instr,
               (unsigned long long)s_tab5_dbf74_total_bytes,
               (unsigned long long)s_tab5_dbf74_total_cycles,
               (unsigned long)s_tab5_dbf74_total_maxbody,
               (unsigned long)s_tab5_dbf74_total_maxbatch);
        printf("PX68K_STREAM580: reg2gv=%lu/%llu ram2gv=%lu/%llu tv2gv=%lu/%llu gv2gv=%lu/%llu ipl2gv=%lu/%llu cycles=%llu maxchunk=%lu same-next=%lu\n",
               (unsigned long)s_tab5_stream576_reg_hits,
               (unsigned long long)s_tab5_stream576_reg_words,
               (unsigned long)s_tab5_stream576_ram_hits,
               (unsigned long long)s_tab5_stream576_ram_words,
               (unsigned long)s_tab5_stream579_tv_hits,
               (unsigned long long)s_tab5_stream579_tv_words,
               (unsigned long)s_tab5_stream576_gv_hits,
               (unsigned long long)s_tab5_stream576_gv_words,
               (unsigned long)s_tab5_stream578_ipl_hits,
               (unsigned long long)s_tab5_stream578_ipl_words,
               (unsigned long long)s_tab5_stream576_cycles,
               (unsigned long)s_tab5_stream576_maxchunk,
               (unsigned long)s_tab5_stream576_same_next);
        printf("PX68K_STREAM580: alias24 hits=%lu words=%llu backend(ram/tv/gv/ipl)=%lu/%lu/%lu/%lu reject src=%lu tvram/mmio/font/other=%lu/%lu/%lu/%lu dst=%lu dst-alias=%lu\n",
               (unsigned long)s_tab5_stream578_alias_hits,
               (unsigned long long)s_tab5_stream578_alias_words,
               (unsigned long)s_tab5_stream578_alias_ram_hits,
               (unsigned long)s_tab5_stream579_alias_tv_hits,
               (unsigned long)s_tab5_stream578_alias_gv_hits,
               (unsigned long)s_tab5_stream578_alias_ipl_hits,
               (unsigned long)s_tab5_stream576_src_reject,
               (unsigned long)s_tab5_stream578_rej_tvram,
               (unsigned long)s_tab5_stream578_rej_mmio,
               (unsigned long)s_tab5_stream578_rej_font,
               (unsigned long)s_tab5_stream578_rej_other,
               (unsigned long)s_tab5_stream576_dst_reject,
               (unsigned long)s_tab5_stream578_dst_alias_hits);
        printf("PX68K_STREAM581_P4: enabled=%d repeat pie=%lu/%llu vec=%llu scalar=%lu/%llu fallback=%lu; copy=scalar-fixed\n",
               s_tab5_stream581_pie_enabled,
               (unsigned long)s_tab5_stream581_repeat_pie_calls,
               (unsigned long long)s_tab5_stream581_repeat_pie_words,
               (unsigned long long)s_tab5_stream581_repeat_vec_words,
               (unsigned long)s_tab5_stream581_repeat_scalar_calls,
               (unsigned long long)s_tab5_stream581_repeat_scalar_words,
               (unsigned long)s_tab5_stream581_pie_fallbacks);

        printf("PX68K_FILL60: bulk-zero calls=%lu loops=%lu guest-instr-elided=%lu bytes=%llu cycles=%llu\n",
               (unsigned long)s_tab5_fill59_calls,
               (unsigned long)s_tab5_fill59_loops,
               (unsigned long)s_tab5_fill59_instr,
               (unsigned long long)s_tab5_fill59_bytes,
               (unsigned long long)s_tab5_fill59_cycles);
        printf("PX68K_CLEAR598G6: cmpa-loop calls=%lu loops=%llu guest-instr-elided=%llu bytes=%llu cycles=%llu maxbatch=%lu\n",
               (unsigned long)s_tab5_clear598g6_calls,
               (unsigned long long)s_tab5_clear598g6_loops,
               (unsigned long long)s_tab5_clear598g6_instr,
               (unsigned long long)s_tab5_clear598g6_bytes,
               (unsigned long long)s_tab5_clear598g6_cycles,
               (unsigned long)s_tab5_clear598g6_maxbatch);
        printf("PX68K_MOVEW571: live scanner RETIRED (5.70 measured zero successful batches)\n");
    }
}

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
    const int is_fdc = (kind_bit == 4u);
    if (remain <= 0) return;

    TAB5_PROF(
        if (kind_bit == 1u) s_tab5_poll58_total_moveand++;
        else if (kind_bit == 2u) s_tab5_poll58_total_btst++;
        else if (kind_bit == 4u) s_tab5_poll59_total_fdc++;
        else if (kind_bit == 8u) s_tab5_poll73_total_ram_moveand++;
        else if (kind_bit == 16u) s_tab5_poll73_total_ram_btst++;
        else if (kind_bit == 32u) s_tab5_poll73_total_ram_cmpi++;
        else if (kind_bit == 64u) s_tab5_poll598f_total_ram_tstw++;
        else if (kind_bit == 128u) s_tab5_poll598g6_total_ram_tstw_abs++;
        s_tab5_poll58_total_skipped += (uint32_t)remain;
        if (is_fdc) s_tab5_poll59_total_fdc_skipped += (uint32_t)remain;
    );

    if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && s_tab5_dispatch_prof_enabled, 0)) {
        if (kind_bit == 1u) s_tab5_poll58_diag_moveand++;
        else if (kind_bit == 2u) s_tab5_poll58_diag_btst++;
        else if (kind_bit == 4u) s_tab5_poll59_diag_fdc++;
        else if (kind_bit == 8u) s_tab5_poll73_diag_ram_moveand++;
        else if (kind_bit == 16u) s_tab5_poll73_diag_ram_btst++;
        else if (kind_bit == 32u) s_tab5_poll73_diag_ram_cmpi++;
        else if (kind_bit == 64u) s_tab5_poll598f_diag_ram_tstw++;
        else if (kind_bit == 128u) s_tab5_poll598g6_diag_ram_tstw_abs++;
        s_tab5_poll58_diag_skipped += (uint32_t)remain;
        s_tab5_poll58_last_pc = loop_pc;
    }

    if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && (s_tab5_poll58_announce_mask & kind_bit) == 0u, 0)) {
        s_tab5_poll58_announce_mask |= kind_bit;
        if (is_fdc) {
            printf("PX68K_POLL73: FDC-STATUS fast-forward ACTIVE kind=MOVE.B+AND+BNE loop=$%06lX; scheduler-resume per slice\n",
                   (unsigned long)(loop_pc & 0x00ffffffu));
        } else if (kind_bit == 1u || kind_bit == 2u) {
            printf("PX68K_POLL73: MFP-GPIP fast-forward ACTIVE kind=%s loop=$%06lX; scheduler-resume per slice\n",
                   (kind_bit == 1u) ? "MOVE.B+AND+BNE" : "BTST+BNE",
                   (unsigned long)(loop_pc & 0x00ffffffu));
        } else {
            const char *kind = (kind_bit == 8u) ? "MOVE.B+AND+BNE"
                              : (kind_bit == 16u) ? "BTST+BNE"
                              : (kind_bit == 32u) ? "CMPI.W+BCS"
                              : (kind_bit == 64u) ? "TST.W(An)+Bcc"
                              : "TST.W abs.l+Bcc";
            printf("PX68K_POLL73: stable-RAM fast-forward ACTIVE kind=%s loop=$%06lX; scheduler-resume per slice\n",
                   kind, (unsigned long)(loop_pc & 0x00ffffffu));
        }
    }

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
        if (__builtin_expect((s_tab5_poll58_observe_mask & 1u) == 0u, 0)) {
            s_tab5_poll58_observe_mask |= 1u;
            printf("PX68K_POLL73: observe-only MOVE.B+AND+BNE loop=$%06lX read=$%06lX (not GPIP/FDC-status)\n",
                   (unsigned long)loop_pc, (unsigned long)addr);
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
        if (__builtin_expect((s_tab5_poll58_observe_mask & 2u) == 0u, 0)) {
            s_tab5_poll58_observe_mask |= 2u;
            printf("PX68K_POLL73: observe-only BTST+BNE loop=$%06lX read=$%06lX (volatile/unknown region)\n",
                   (unsigned long)loop_pc, (unsigned long)addr);
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
        if (__builtin_expect((s_tab5_poll58_observe_mask & 4u) == 0u, 0)) {
            s_tab5_poll58_observe_mask |= 4u;
            printf("PX68K_POLL73: observe-only CMPI.W+BCS loop=$%06lX read=$%06lX (volatile/unknown region)\n",
                   (unsigned long)loop_pc, (unsigned long)addr);
        }
    }
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

    if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && s_tab5_opcode_profile_enabled, 0)) {
        s_tab5_opcode_counts[op_clr] += n * 2u;
        s_tab5_opcode_counts[op_sub] += n;
        s_tab5_opcode_counts[op_add] += n;
        s_tab5_opcode_counts[op_bpl] += n;
        s_tab5_opcode_total += n * 5u;
    }

    USE_CYCLES((int)(n * (uint32_t)loop_cycles));

    TAB5_PROF(
        ++s_tab5_loop598g11_calls;
        s_tab5_loop598g11_loops += n;
        s_tab5_loop598g11_instr += (uint64_t)n * 5u;
        s_tab5_loop598g11_zero_bytes += (uint64_t)n * 4u;
        s_tab5_loop598g11_span_bytes += span;
        s_tab5_loop598g11_cycles += (uint64_t)n * (uint32_t)loop_cycles;
        if (n > s_tab5_loop598g11_maxbatch) s_tab5_loop598g11_maxbatch = n;
    );

    if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && !s_tab5_loop598g11_announced, 0)) {
        s_tab5_loop598g11_announced = 1;
        printf("PX68K_LOOP598G11: sparse CLR.Wx2/SUBQ/ADDQ/BPL batch ACTIVE "
               "pc=$%06lX-$%06lX; A0 ordinary-RAM stride=8 zero=4; scheduler-bounded\n",
               (unsigned long)loop_pc, (unsigned long)branch_pc);
    }
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

    if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && s_tab5_opcode_profile_enabled, 0)) {
        /* Preserve the logical guest profile even though n future iterations
         * are executed as one host bulk operation.  The current architectural
         * iteration was already counted by the main dispatch loop. */
        s_tab5_opcode_counts[clr] += n;
        s_tab5_opcode_counts[cmpa] += n;
        s_tab5_opcode_counts[branch_op] += n;
        s_tab5_opcode_total += n * 3u;
    }

    USE_CYCLES((int)(n * (uint32_t)loop_cycles));

    TAB5_PROF(
        ++s_tab5_clear598g6_calls;
        s_tab5_clear598g6_loops += n;
        s_tab5_clear598g6_instr += (uint64_t)n * 3u;
        s_tab5_clear598g6_bytes += bytes;
        s_tab5_clear598g6_cycles += (uint64_t)n * (uint32_t)loop_cycles;
        if (n > s_tab5_clear598g6_maxbatch) s_tab5_clear598g6_maxbatch = n;
    );

    if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && !s_tab5_clear598g6_announced, 0)) {
        s_tab5_clear598g6_announced = 1;
        printf("PX68K_CLEAR598G6: CLR.L(A%u)+/CMPA.L A%u,A%u/BNE bulk-clear ACTIVE "
               "pc=$%06lX-$%06lX; ordinary RAM + scheduler boundary; PIE-selected zero backend\n",
               dst_a, end_a, dst_a,
               (unsigned long)loop_pc, (unsigned long)branch_pc);
    }
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

    TAB5_PROF(
        s_tab5_dbf73_total_hits++;
        s_tab5_dbf73_total_loops += n;
        s_tab5_dbf73_total_cycles += (uint64_t)n * (uint32_t)per_taken;
        if (n > s_tab5_dbf73_total_maxbatch) s_tab5_dbf73_total_maxbatch = n;
    );
    if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && s_tab5_dispatch_prof_enabled, 0)) {
        s_tab5_dbf73_diag_hits++;
        s_tab5_dbf73_diag_loops += n;
        if (n > s_tab5_dbf73_diag_maxbatch) s_tab5_dbf73_diag_maxbatch = n;
    }
    if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && !s_tab5_dbf73_announced, 0)) {
        s_tab5_dbf73_announced = 1;
        printf("PX68K_DBF73: pure DBF self-loop fast-forward ACTIVE pc=$%06lX D%u; batch within current slice\n",
               (unsigned long)branch_pc, cnt_d);
    }
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
    if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && (s_tab5_opcode_profile_enabled || s_tab5_dispatch_prof_enabled), 0)) return 0;
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

    TAB5_PROF(
        ++s_tab5_dbf74_total_hits;
        s_tab5_dbf74_total_loops += n;
        s_tab5_dbf74_total_instr += (uint64_t)n * (uint64_t)(body_ops + 1u);
        s_tab5_dbf74_total_bytes += bytes;
        s_tab5_dbf74_total_cycles += (uint64_t)n * (uint32_t)loop_cycles;
        if (body_ops > s_tab5_dbf74_total_maxbody) s_tab5_dbf74_total_maxbody = body_ops;
        if (n > s_tab5_dbf74_total_maxbatch) s_tab5_dbf74_total_maxbatch = n;
    );

    if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && s_tab5_dispatch_prof_enabled, 0)) {
        ++s_tab5_dbf74_diag_hits;
        s_tab5_dbf74_diag_loops += n;
        if (body_ops > s_tab5_dbf74_diag_maxbody) s_tab5_dbf74_diag_maxbody = body_ops;
        if (n > s_tab5_dbf74_diag_maxbatch) s_tab5_dbf74_diag_maxbatch = n;
    }

    if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && (s_tab5_dbf74_announced_mask & unit_bytes) == 0u, 0)) {
        s_tab5_dbf74_announced_mask |= unit_bytes;
        printf("PX68K_DBF74: repeated MOVE.%c store-body fast-forward ACTIVE body=%lu pc=$%06lX-$%06lX D%u->A%u DBF D%u; scheduler boundary preserved\n",
               (unit_bytes == 2u) ? 'W' : 'L', (unsigned long)body_ops,
               (unsigned long)loop_pc, (unsigned long)(branch_pc - 2u),
               src_d, dst_a, cnt_d);
    }
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

    if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && (s_tab5_opcode_profile_enabled || s_tab5_dispatch_prof_enabled), 0)) return;
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

    TAB5_PROF(
        s_tab5_fill59_calls++;
        s_tab5_fill59_loops += n;
        s_tab5_fill59_instr += n * 5u;
        s_tab5_fill59_bytes += bytes;
        s_tab5_fill59_cycles += (uint64_t)n * (uint32_t)loop_cycles;
    );
}

static const char *tab5_opcode_family(uint16_t op)
{
    if ((op & 0xf100u) == 0x7000u) return "MOVEQ";
    if ((op & 0xf000u) == 0x6000u) return ((op & 0x0f00u) == 0x0100u) ? "BSR" : "Bcc/BRA";
    switch ((op >> 12) & 0x0f) {
        case 0x0: return "IMM/BIT";
        case 0x1: return "MOVE.B";
        case 0x2: return "MOVE.L";
        case 0x3: return "MOVE.W";
        case 0x4: return "MISC";
        case 0x5: return "ADDQ/SUBQ/Scc";
        case 0x7: return "MOVEQ";
        case 0x8: return "OR/DIV";
        case 0x9: return "SUB";
        case 0xa: return "A-LINE";
        case 0xb: return "CMP/EOR";
        case 0xc: return "AND/MUL/EXG";
        case 0xd: return "ADD";
        case 0xe: return "SHIFT/ROT";
        case 0xf: return "F-LINE";
        default:  return "OTHER";
    }
}

static void tab5_piecpu598g4b_dump(void)
{
    const tab5_piecpu598g4b_obs_t * const o = &s_tab5_piecpu598g4b;
    printf("PX68K_PIECPU598G7: candidate sample MOVEM=%lu W/L=%lu/%lu R2M/M2R=%lu/%lu regs=%lu bytes=%lu R2M/M2Rbytes=%lu/%lu maxregs=%lu predec/postinc=%lu/%lu | post-store W/L=%lu/%lu post-copy W/L=%lu/%lu\n",
           (unsigned long)o->movem_total,
           (unsigned long)o->movem_w, (unsigned long)o->movem_l,
           (unsigned long)o->movem_r2m, (unsigned long)o->movem_m2r,
           (unsigned long)o->movem_regs, (unsigned long)o->movem_bytes,
           (unsigned long)o->movem_r2m_bytes, (unsigned long)o->movem_m2r_bytes,
           (unsigned long)o->movem_max_regs,
           (unsigned long)o->movem_predec, (unsigned long)o->movem_postinc,
           (unsigned long)o->store_w_post, (unsigned long)o->store_l_post,
           (unsigned long)o->copy_w_post, (unsigned long)o->copy_l_post);
    printf("PX68K_PIECPU598G7: candidate sample memory-op CLR=%lu OR=%lu AND=%lu EOR=%lu SHIFT=%lu | proven-batch DBF74 bytes=%llu loops=%llu STREAM repeatPIE words=%llu RAMcopy=%llu TVcopy=%llu IPLcopy=%llu GVcopy=%llu\n",
           (unsigned long)o->clr_mem, (unsigned long)o->or_mem,
           (unsigned long)o->and_mem, (unsigned long)o->eor_mem,
           (unsigned long)o->mem_shift,
           (unsigned long long)s_tab5_dbf74_total_bytes,
           (unsigned long long)s_tab5_dbf74_total_loops,
           (unsigned long long)s_tab5_stream581_repeat_pie_words,
           (unsigned long long)s_tab5_stream576_ram_words,
           (unsigned long long)s_tab5_stream579_tv_words,
           (unsigned long long)s_tab5_stream578_ipl_words,
           (unsigned long long)s_tab5_stream576_gv_words);
}

static void tab5_opcode_profile_dump(void)
{
    enum { TOPN = 20 };
    uint32_t top_count[TOPN] = {0};
    uint16_t top_op[TOPN] = {0};
    uint32_t family[16] = {0};
    unsigned op;

    if (s_tab5_opcode_total == 0) return;

    for (op = 0; op < 0x10000u; ++op) {
        uint32_t c = s_tab5_opcode_counts[op];
        unsigned i;
        if (!c) continue;
        family[(op >> 12) & 15u] += c;
        if (c <= top_count[TOPN - 1]) continue;
        for (i = 0; i < TOPN; ++i) {
            if (c > top_count[i]) {
                unsigned j;
                for (j = TOPN - 1; j > i; --j) {
                    top_count[j] = top_count[j - 1];
                    top_op[j] = top_op[j - 1];
                }
                top_count[i] = c;
                top_op[i] = (uint16_t)op;
                break;
            }
        }
    }

    printf("PX68K_M68K: OPCODE PROFILE total=%lu top20 (sample frame; profiling overhead excluded from optimization judgement)\n",
           (unsigned long)s_tab5_opcode_total);
    for (op = 0; op < TOPN && top_count[op]; ++op) {
        const uint32_t pct_x100 = s_tab5_opcode_total
            ? (top_count[op] * 10000u) / s_tab5_opcode_total : 0u;
        printf("PX68K_M68K: OP%02u $%04X count=%lu pct=%lu.%02lu family=%s handler=%p\n",
               op + 1, (unsigned)top_op[op], (unsigned long)top_count[op],
               (unsigned long)(pct_x100 / 100u), (unsigned long)(pct_x100 % 100u),
               tab5_opcode_family(top_op[op]),
               (void *)m68ki_instruction_jump_table[top_op[op]]);
    }
    printf("PX68K_M68K: FAMILY nibbles 0..F =");
    for (op = 0; op < 16; ++op)
        printf(" %X:%lu", op, (unsigned long)family[op]);
    printf("\n");
}

void m68k_tab5_opcode_profile_set(int enabled)
{
    enabled = enabled ? 1 : 0;
    if (enabled && !s_tab5_opcode_profile_enabled) {
        memset(s_tab5_opcode_counts, 0, sizeof(s_tab5_opcode_counts));
        s_tab5_opcode_total = 0;
        tab5_movew65b_reset();
        tab5_piecpu598g4b_reset();
        s_tab5_opcode_profile_enabled = 1;
    } else if (!enabled && s_tab5_opcode_profile_enabled) {
        s_tab5_opcode_profile_enabled = 0;
        tab5_opcode_profile_dump();
        tab5_movew65b_dump();
        tab5_piecpu598g4b_dump();
    }
}
#else
void m68k_tab5_opcode_profile_set(int enabled) { (void)enabled; }
void m68k_tab5_dispatch_profile_set(int enabled) { (void)enabled; }
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

#ifdef ESP_PLATFORM
DRAM_ATTR jmp_buf m68ki_bus_error_jmp_buf;
#else
jmp_buf m68ki_bus_error_jmp_buf;
#endif

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

#if !defined(ESP_PLATFORM) || !PX68K_TAB5_SKIP_DEAD_BERR_ROLLBACK
		m68ki_check_bus_error_trap();
#endif

		/* Main loop.  Keep going until we run out of clock cycles */
		do
		{
			/* Set tracing accodring to T1. (T0 is done inside instruction) */
			m68ki_trace_t1(); /* auto-disable (see m68kcpu.h) */

			/* Set the address space for reads */
			m68ki_use_data_space(); /* auto-disable (see m68kcpu.h) */

			/* Call external hook to peek at CPU */
			m68ki_instr_hook(REG_PC); /* auto-disable (see m68kcpu.h) */

			/* Record previous program counter */
			REG_PPC = REG_PC;

			/* Record previous D/A register state only when Musashi bus-error
			 * rollback is actually reachable.  Build 5.64 removes this 64-byte
			 * copy from every guest instruction on the Tab5 integration. */
#if !defined(ESP_PLATFORM) || !PX68K_TAB5_SKIP_DEAD_BERR_ROLLBACK
#ifdef ESP_PLATFORM
			if (__builtin_expect(s_tab5_xespv_snapshot_enabled, 1))
				tab5_xespv_copy64(REG_DA_SAVE, REG_DA);
			else
				tab5_scalar_copy64(REG_DA_SAVE, REG_DA);
#else
			for (int tab5_i = 0; tab5_i < 16; ++tab5_i) REG_DA_SAVE[tab5_i] = REG_DA[tab5_i];
#endif
#endif

			/* Read an instruction and dispatch it.
			 *
			 * Build 5.72 (ESP32-P4): flatten the hottest opcode-fetch call.
			 * m68ki_read_imm_16() used to call px68k_m68k_fetch_16() once for
			 * every guest instruction.  Keep the exact 68000 address-error and
			 * PC-update order, but perform the common RAM/IPL word fetch directly
			 * in this execute loop.  Extension/immediate words intentionally keep
			 * the existing helper path so this build measures opcode-call removal
			 * in isolation. */
#ifdef ESP_PLATFORM
			{
				uint32_t tab5_fetch_pc = REG_PC;
				uint16_t tab5_fetch_word;

				m68ki_set_fc(FLAG_S | FUNCTION_CODE_USER_PROGRAM);
				m68ki_check_address_error(tab5_fetch_pc, MODE_READ,
				                          FLAG_S | FUNCTION_CODE_USER_PROGRAM);
				REG_PC = tab5_fetch_pc + 2u;
				tab5_fetch_pc = ADDRESS_68K(tab5_fetch_pc);

				if (__builtin_expect(tab5_fetch_pc <= 0x00bffffeu, 1)) {
					__builtin_memcpy(&tab5_fetch_word, MEM + tab5_fetch_pc,
					                 sizeof(tab5_fetch_word));
					BusErrFlag = 0;
					REG_IR = (uint32_t)tab5_fetch_word;
				} else if (__builtin_expect(tab5_fetch_pc >= 0x00fc0000u &&
				                               tab5_fetch_pc <= 0x00fffffeu, 0)) {
					__builtin_memcpy(&tab5_fetch_word,
					                 IPL + (tab5_fetch_pc & 0x0003ffffu),
					                 sizeof(tab5_fetch_word));
					BusErrFlag = 0;
					REG_IR = (uint32_t)tab5_fetch_word;
				} else {
					REG_IR = (uint32_t)cpu_readmem24_word(tab5_fetch_pc);
				}
			}
#else
			REG_IR = m68ki_read_imm_16();
#endif
#ifdef ESP_PLATFORM
			if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && s_tab5_opcode_profile_enabled, 0)) {
				s_tab5_opcode_counts[REG_IR]++;
				s_tab5_opcode_total++;
				tab5_movew65b_observe((uint16_t)REG_IR, REG_PPC);
				tab5_piecpu598g4b_observe((uint16_t)REG_IR);
			}

			{
				const uint16_t op = (uint16_t)REG_IR;
				const unsigned ci = tab5_dispatch_index(op);
				tab5_m68k_dispatch_entry_t * const ce = &s_tab5_dispatch_cache[ci];
				const uint32_t sig = ce->sig_cycles;
				tab5_m68k_handler_t handler;
				uint32_t instr_cycles;
				uint32_t post_flags;

				if (__builtin_expect((sig & 0x8000ffffu) == (0x80000000u | (uint32_t)op), 1)) {
					handler = ce->handler;
					instr_cycles = (sig >> 16) & 0xffu;
					post_flags = (sig >> 24) & 0x7fu;
					if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && s_tab5_dispatch_prof_enabled, 0)) s_tab5_dispatch_hits++;
				} else {
					const unsigned ci2 = tab5_dispatch_l2_index(op);
					tab5_m68k_dispatch_entry_t * const se = &s_tab5_dispatch_l2[ci2];
					const uint32_t sig2 = se->sig_cycles;
					if (__builtin_expect((sig2 & 0x8000ffffu) == (0x80000000u | (uint32_t)op), 1)) {
						handler = se->handler;
						instr_cycles = (sig2 >> 16) & 0xffu;
						post_flags = (sig2 >> 24) & 0x7fu;
						ce->handler = handler;
						ce->sig_cycles = sig2;
						if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && s_tab5_dispatch_prof_enabled, 0)) s_tab5_dispatch_l2_hits++;
					} else {
						handler = m68ki_instruction_jump_table[op];
						instr_cycles = CYC_INSTRUCTION[op];
						post_flags = tab5_post585_flags(op);
						const uint32_t newsig = 0x80000000u | ((post_flags & 0x7fu) << 24) |
						                        ((instr_cycles & 0xffu) << 16) | (uint32_t)op;
						se->handler = handler;
						se->sig_cycles = newsig;
						ce->handler = handler;
						ce->sig_cycles = newsig;
						if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && s_tab5_dispatch_prof_enabled, 0)) s_tab5_dispatch_misses++;
					}
				}

				/* Build 5.98f: one rare metadata gate covers Bcc.B/W, DBcc and
				 * concrete register-only hot kernels.  Ordinary instructions keep the
				 * unchanged direct handler-call path. */
				if (__builtin_expect(post_flags == 0u, 1)) {
					handler();
					USE_CYCLES(instr_cycles);
				} else {
					if (post_flags & TAB5_POST598F_BNE_FAST) {
						if ((op & 0x00ffu) != 0u) {
							/* Preserve the proven 5.98d/5.98e byte-BNE hot path. */
							if (__builtin_expect(FLAG_Z != 0u, 1)) {
								REG_PC += MAKE_INT_8(op & 0x00ffu);
							} else
								USE_CYCLES(CYC_BCC_NOTAKE_B);
						} else {
							tab5_bcc598f_exec(op, FLAG_Z != 0u);
							if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && s_tab5_dispatch_prof_enabled, 0)) s_tab5_fast598f_bccw++;
						}
					} else if (post_flags & TAB5_POST598F_BEQ_FAST) {
						if ((op & 0x00ffu) != 0u) {
							if (__builtin_expect(FLAG_Z == 0u, 1))
								REG_PC += MAKE_INT_8(op & 0x00ffu);
							else
								USE_CYCLES(CYC_BCC_NOTAKE_B);
						} else {
							tab5_bcc598f_exec(op, FLAG_Z == 0u);
							if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && s_tab5_dispatch_prof_enabled, 0)) s_tab5_fast598f_bccw++;
						}
					} else if (post_flags & TAB5_POST598F_BCC_FAST) {
						tab5_bcc598f_exec(op, tab5_bcc598f_taken(op));
						if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && s_tab5_dispatch_prof_enabled, 0) && (op & 0x00ffu) == 0u)
							s_tab5_fast598f_bccw++;
					} else if (post_flags & TAB5_POST585_DBF) {
						(void)tab5_dbcc598f_exec(op);
						if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && s_tab5_dispatch_prof_enabled, 0)) s_tab5_fast598f_dbcc++;
					} else if (post_flags & TAB5_POST598F_CORE_FAST) {
						if (((op & 0xfff8u) == 0x48e0u || (op & 0xfff8u) == 0x4cd8u) &&
						    !tab5_movem598g9_try(op))
							handler();
						else if ((op & 0xfff8u) != 0x48e0u && (op & 0xfff8u) != 0x4cd8u)
							tab5_core598f_exec(op);
						if (__builtin_expect(PX68K_TAB5_PERF_PROFILE && s_tab5_dispatch_prof_enabled, 0)) {
							s_tab5_fast598f_core++;
							if ((op & 0xfff8u) == 0x4a50u) s_tab5_fast598f_core_mem++;
							if ((op & 0xf1f8u) == 0x1000u) s_tab5_fast598g7_moveb_rr++;
							if ((op & 0xf1f8u) == 0x30c0u) s_tab5_fast598g7_movew_pi++;
							if ((op & 0xf1f8u) == 0x20c0u) s_tab5_fast598g9_movel_pi++;
							if ((op & 0xf1f8u) == 0x20d8u) s_tab5_fast598g10_movel_pipi++;
							if ((op & 0xfff8u) == 0x0800u) s_tab5_fast598g10_btst_imm_d++;
							if ((op & 0xfff8u) == 0x4298u) s_tab5_fast598g10_clrl_pi++;
							if ((op & 0xfff8u) == 0x48c0u) s_tab5_fast598g10_extl++;
							if ((op & 0xfff8u) == 0x4258u) s_tab5_fast598g9_clrw_pi++;
							if ((op & 0xf1f8u) == 0x5048u || (op & 0xf1f8u) == 0x5088u)
								s_tab5_fast598g9_addq_a++;
						}
					} else {
						handler();
					}
					USE_CYCLES(instr_cycles);

					/* Build 5.98g6: the current BNE cycles are now charged, so any
					 * future CLR/CMPA/BNE iterations may be bounded exactly by the
					 * remaining scheduler slice. */
					if (__builtin_expect((post_flags & TAB5_POST598F_BNE_FAST) &&
					                     ((op & 0x00ffu) == 0xfau) && FLAG_Z != 0u, 0))
						tab5_clear598g6_try_cmpa_bne(op);

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
#if PX68K_TAB5_SKIP_DEAD_BERR_ROLLBACK
		printf("PX68K_BERR64: Musashi bus-error rollback preparation BYPASSED; address-error emulation retained\n");
#else
		tab5_xespv_snapshot_selfcheck();
#endif
		s_tab5_stream581_pie_enabled = GVRAM_P4StreamSelfcheck();
		printf("PX68K_STREAM581: Build 5.89a P4 PIE/XespV repeat backend self-check %s; repeat=%s copy=SCALAR\n",
		       s_tab5_stream581_pie_enabled ? "PASS" : "FAIL",
		       s_tab5_stream581_pie_enabled ? "PIE-128" : "SCALAR-FALLBACK");
		printf("PX68K_POST585: fused dispatch-metadata post-op gate ACTIVE; normal instruction path tests one flag word, stream/poll/DBF recognizers unchanged\n");
		printf("PX68K_BRANCH598F: Bcc.B/Bcc.W + DBcc inline via existing dispatch metadata gate; exact cycles/extension fetch retained\n");
		printf("PX68K_CORE598F: measured register-only fast paths ACTIVE: MOVE.W/L rr, MOVEQ, ADD.W rr, AND rr/imm, TST.W, SWAP, ADDQ/SUBQ.W, ASR.W/L\n");
		printf("PX68K_CPU598G10: g9 hot paths + MOVE.L (An)+,(Am)+ / BTST #imm,Dn / CLR.L (An)+ / EXT.L Dn inline armed; generated-handler semantics retained\n");
		printf("PX68K_LOOP598G11: exact CLR.W(A0)+ x2 / SUBQ.W D0 / ADDQ #4,A0 / BPL -10 ordinary-RAM batch armed; scheduler boundary + final exit preserved\n");
		printf("PX68K_DISPATCH598G9: TCM L1=%u entries + internal-DRAM L2=%u entries (%u bytes) armed before PSRAM metadata fallback\n",
		       (unsigned)TAB5_M68K_DISPATCH_CACHE_SIZE, (unsigned)TAB5_M68K_DISPATCH_L2_SIZE,
		       (unsigned)sizeof(s_tab5_dispatch_l2));
		printf("PX68K_POLL598F: stable ordinary-RAM TST.W(An)+BNE/BEQ scheduler-bounded fast-forward armed\n");
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

/* Trigger a Bus Error exception */
void m68k_pulse_bus_error(void)
{
	m68ki_exception_bus_error();
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
