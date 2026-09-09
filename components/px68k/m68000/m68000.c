/******************************************************************************

	m68000.c

	M68000 CPU���󥿥ե������ؿ�E

******************************************************************************/

/*
 * PX68K source modified for the Tab5 port.
 * Intent: Integrate Musashi with the Tab5 standalone scheduler and memory layout while preserving authoritative 68000 architectural state.
 * Layer8 Aug/17/2026
 */
#include "m68000.h"
#include <string.h>

#if defined (HAVE_CYCLONE)
struct Cyclone m68k;
#elif defined (HAVE_C68K)
#include "c68k/c68k.h"
#elif defined (HAVE_MUSASHI)
#include "musashi/m68k.h"
#include "musashi/m68kcpu.h"
#endif /* HAVE_C68K */ /* HAVE_MUSASHI */

#include "../x68k/x68kmemory.h"
#include "../x68k/irqh.h"

#ifdef ESP_PLATFORM
#include "esp_attr.h"
#ifdef TCM_DRAM_ATTR
#define PX68K_CPUHOT TCM_DRAM_ATTR
#else
#define PX68K_CPUHOT DRAM_ATTR
#endif
#else
#define PX68K_CPUHOT
#endif

#ifdef ESP_PLATFORM
static void px68k_p4_isa_banner(void)
{
    int zba = 0, zbb = 0, zbs = 0, compressed = 0;
#ifdef __riscv_zba
    zba = 1;
#endif
#ifdef __riscv_zbb
    zbb = 1;
#endif
#ifdef __riscv_zbs
    zbs = 1;
#endif
#ifdef __riscv_compressed
    compressed = 1;
#endif
    printf("PX68K_P4CPU: Build 5.62 ISA compiler-view RV32 zba=%d zbb=%d zbs=%d c=%d; TCM dispatch-cache=256 entries\n",
           zba, zbb, zbs, compressed);
}
#endif

#if defined (HAVE_MUSASHI)
#include <stdio.h>
static unsigned int px68k_illegal_count;
static int px68k_illegal_diag(int opcode)
{
    const unsigned int ppc = m68k_get_reg(NULL, M68K_REG_PPC) & 0x00ffffffu;
    const unsigned int pc  = m68k_get_reg(NULL, M68K_REG_PC)  & 0x00ffffffu;
    if (px68k_illegal_count < 8u)
    {
        ++px68k_illegal_count;
        printf("PX68K_CPU: *** ILLEGAL #%u PPC=$%06X PC=$%06X OPCODE=$%04X D2=$%08X A1=$%08X A7=$%08X ***\n",
               px68k_illegal_count, ppc, pc, (unsigned int)opcode & 0xffffu,
               m68k_get_reg(NULL, M68K_REG_D2),
               m68k_get_reg(NULL, M68K_REG_A1),
               m68k_get_reg(NULL, M68K_REG_A7));
    }
    return 0; /* keep normal X68000 illegal-instruction exception behavior */
}
#endif

PX68K_CPUHOT int m68000_ICountBk;
PX68K_CPUHOT int ICount;

int m68000_StateAction(StateMem *sm, int load, int data_only)
{
#ifdef HAVE_C68K
	int ret = 0;
	uint32_t pc = 0;
	SFORMAT StateRegs[] = 
	{
		SFVARN(C68K.D[0], "D0"),
		SFVARN(C68K.D[1], "D1"),
		SFVARN(C68K.D[2], "D2"),
		SFVARN(C68K.D[3], "D3"),
		SFVARN(C68K.D[4], "D4"),
		SFVARN(C68K.D[5], "D5"),
		SFVARN(C68K.D[6], "D6"),
		SFVARN(C68K.D[7], "D7"),

		SFVARN(C68K.A[0], "A0"),
		SFVARN(C68K.A[1], "A1"),
		SFVARN(C68K.A[2], "A2"),
		SFVARN(C68K.A[3], "A3"),
		SFVARN(C68K.A[4], "A4"),
		SFVARN(C68K.A[5], "A5"),
		SFVARN(C68K.A[6], "A6"),
		SFVARN(C68K.A[7], "A7"),

		SFVARN(C68K.flag_C, "flag_C"),
		SFVARN(C68K.flag_V, "flag_V"),
		SFVARN(C68K.flag_notZ, "flag_notZ"),
		SFVARN(C68K.flag_X, "flag_X"),
		SFVARN(C68K.flag_I, "flag_I"),
		SFVARN(C68K.flag_S, "flag_S"),

		SFVARN(C68K.USP, "USP"),

		SFVARN(pc, "PC"),

		SFVARN(C68K.Status, "status"),
		SFVARN(C68K.IRQLine, "IRQLine"),

		SFVARN(C68K.CycleToDo, "CycleToDo"),
		SFVARN(C68K.CycleIO, "CycleIO"),
		SFVARN(C68K.CycleSup, "CycleSup"),
		SFVARN(C68K.dirty1, "dirtyflag"),

		SFEND
	};

	if (!load)
		pc = m68000_get_reg(M68K_PC);

	ret = PX68KSS_StateAction(sm, load, data_only, StateRegs, "X68K_CPU", false);

	if (load)
		m68000_set_reg(M68K_PC, pc);

	return ret;
#endif
#ifdef HAVE_MUSASHI
	int ret = 0;
	uint32_t tmp32[20];

	SFORMAT StateRegs[] =
	{
		SFVARN(tmp32[0], "M68K_REG_D0"),
		SFVARN(tmp32[1], "M68K_REG_D1"),
		SFVARN(tmp32[2], "M68K_REG_D2"),
		SFVARN(tmp32[3], "M68K_REG_D3"),
		SFVARN(tmp32[4], "M68K_REG_D4"),
		SFVARN(tmp32[5], "M68K_REG_D5"),
		SFVARN(tmp32[6], "M68K_REG_D6"),
		SFVARN(tmp32[7], "M68K_REG_D7"),
		SFVARN(tmp32[8], "M68K_REG_A0"),
		SFVARN(tmp32[9], "M68K_REG_A1"),
		SFVARN(tmp32[10], "M68K_REG_A2"),
		SFVARN(tmp32[11], "M68K_REG_A3"),
		SFVARN(tmp32[12], "M68K_REG_A4"),
		SFVARN(tmp32[13], "M68K_REG_A5"),
		SFVARN(tmp32[14], "M68K_REG_A6"),
		SFVARN(tmp32[15], "M68K_REG_A7"),
		SFVARN(tmp32[16], "M68K_REG_PC"),
		SFVARN(tmp32[17], "M68K_REG_SR"),
		SFVARN(tmp32[18], "M68K_REG_USP"),
		SFVARN(tmp32[19], "M68K_REG_ISP"),

		SFVAR(m68ki_cpu.c_flag),
		SFVAR(m68ki_cpu.v_flag),
		SFVAR(m68ki_cpu.not_z_flag),
		SFVAR(m68ki_cpu.n_flag),
		SFVAR(m68ki_cpu.x_flag),
		SFVAR(m68ki_cpu.m_flag),
		SFVAR(m68ki_cpu.s_flag),
		
		SFVAR(m68ki_cpu.int_level),
		SFVAR(m68ki_cpu.stopped),
		SFVAR(m68ki_remaining_cycles),

		SFEND
	};

	if (!load)
	{
		tmp32[0] = m68k_get_reg(NULL, M68K_REG_D0);
		tmp32[1] = m68k_get_reg(NULL, M68K_REG_D1);
		tmp32[2] = m68k_get_reg(NULL, M68K_REG_D2);
		tmp32[3] = m68k_get_reg(NULL, M68K_REG_D3);
		tmp32[4] = m68k_get_reg(NULL, M68K_REG_D4);
		tmp32[5] = m68k_get_reg(NULL, M68K_REG_D5);
		tmp32[6] = m68k_get_reg(NULL, M68K_REG_D6);
		tmp32[7] = m68k_get_reg(NULL, M68K_REG_D7);
		tmp32[8] = m68k_get_reg(NULL, M68K_REG_A0);
		tmp32[9] = m68k_get_reg(NULL, M68K_REG_A1);
		tmp32[10] = m68k_get_reg(NULL, M68K_REG_A2);
		tmp32[11] = m68k_get_reg(NULL, M68K_REG_A3);
		tmp32[12] = m68k_get_reg(NULL, M68K_REG_A4);
		tmp32[13] = m68k_get_reg(NULL, M68K_REG_A5);
		tmp32[14] = m68k_get_reg(NULL, M68K_REG_A6);
		tmp32[15] = m68k_get_reg(NULL, M68K_REG_A7);
		tmp32[16] = m68k_get_reg(NULL, M68K_REG_PC);
		tmp32[17] = m68k_get_reg(NULL, M68K_REG_SR);
		tmp32[18] = m68k_get_reg(NULL, M68K_REG_USP);
		tmp32[19] = m68k_get_reg(NULL, M68K_REG_ISP);
	}

	ret = PX68KSS_StateAction(sm, load, data_only, StateRegs, "MUSASHI_CPU", false);

	if (load)
	{
		m68k_set_reg(M68K_REG_D0, tmp32[0]);
		m68k_set_reg(M68K_REG_D1, tmp32[1]);
		m68k_set_reg(M68K_REG_D2, tmp32[2]);
		m68k_set_reg(M68K_REG_D3, tmp32[3]);
		m68k_set_reg(M68K_REG_D4, tmp32[4]);
		m68k_set_reg(M68K_REG_D5, tmp32[5]);
		m68k_set_reg(M68K_REG_D6, tmp32[6]);
		m68k_set_reg(M68K_REG_D7, tmp32[7]);
		m68k_set_reg(M68K_REG_A0, tmp32[8]);
		m68k_set_reg(M68K_REG_A1, tmp32[9]);
		m68k_set_reg(M68K_REG_A2, tmp32[10]);
		m68k_set_reg(M68K_REG_A3, tmp32[11]);
		m68k_set_reg(M68K_REG_A4, tmp32[12]);
		m68k_set_reg(M68K_REG_A5, tmp32[13]);
		m68k_set_reg(M68K_REG_A6, tmp32[14]);
		m68k_set_reg(M68K_REG_A7, tmp32[15]);
		m68k_set_reg(M68K_REG_PC, tmp32[16]);
		m68k_set_reg(M68K_REG_SR, tmp32[17]);
		m68k_set_reg(M68K_REG_USP, tmp32[18]);
		m68k_set_reg(M68K_REG_ISP, tmp32[19]);
	};

	return ret;
#endif
}

#if defined (HAVE_CYCLONE)

unsigned int read8(unsigned int a) {
	return (unsigned int) cpu_readmem24(a);
}

unsigned int read16(unsigned int a) {
	return (unsigned int) cpu_readmem24_word(a);
}

unsigned int MyCheckPc(unsigned int pc)
{
  pc-= m68k.membase; /* Get the real program counter */
  if (pc <= 0xbfffff) 			       					{ m68k.membase=(int) MEM; return m68k.membase+pc; }
  if ((pc >= 0xfc0000) && (pc <= 0xffffff))	{ m68k.membase=(int) IPL - 0xfc0000; return m68k.membase+pc; }
  if ((pc >= 0xc00000) && (pc <= 0xc7ffff)) m68k.membase=(int) GVRAM - 0xc00000;
  if ((pc >= 0xe00000) && (pc <= 0xe7ffff))	m68k.membase=(int) TVRAM - 0xe00000;
  if ((pc >= 0xea0000) && (pc <= 0xea1fff))	m68k.membase=(int) SCSIIPL - 0xea0000;
  if ((pc >= 0xed0000) && (pc <= 0xed3fff))	m68k.membase=(int) SRAM - 0xed0000;
  if ((pc >= 0xf00000) && (pc <= 0xfbffff))	m68k.membase=(int) FONT - 0xf00000;
  return m68k.membase+pc; /* New program counter */
}

#elif defined (HAVE_MUSASHI)
/*
 * Build 5.35 (ESP32-P4): Musashi direct RAM/IPL memory fast path.
 *
 * The generic PX68K memory wrapper is byte-oriented: even an aligned 16-bit
 * RAM opcode fetch used to travel through cpu_readmem24_word() and rm_main()
 * twice.  On Musashi that path is hit for virtually every instruction.
 *
 * PX68K stores RAM and IPL as word-swapped byte arrays on little-endian
 * hosts: guest byte address A maps to host byte A^1.  The fast paths below
 * reproduce exactly the same representation while bypassing only the generic
 * address decoder for plain RAM and IPL accesses.  MMIO/GVRAM/TVRAM and any
 * boundary-crossing access still use the original functions.
 */
#ifdef ESP_PLATFORM
#define PX68K_TAB5_FASTMEM 1
#else
#define PX68K_TAB5_FASTMEM 0
#endif

#if PX68K_TAB5_FASTMEM
/* Build 5.54: MEM/IPL are stored in PX68K's 68000 word-swapped layout.
 * On little-endian ESP32-P4 an aligned native 16-bit load therefore returns
 * the guest 16-bit word directly. __builtin_memcpy is alias-safe and GCC
 * lowers these fixed 2-byte copies to native halfword loads/stores. */
static inline __attribute__((always_inline)) uint32_t px68k_native_word_load(const uint8_t *p)
{
    uint16_t v;
    __builtin_memcpy(&v, p, sizeof(v));
    return (uint32_t)v;
}

static inline __attribute__((always_inline)) uint32_t px68k_native_long_load(const uint8_t *p)
{
    const uint32_t hi = px68k_native_word_load(p);
    const uint32_t lo = px68k_native_word_load(p + 2);
    return (hi << 16) | lo;
}

static inline __attribute__((always_inline)) void px68k_native_word_store(uint8_t *p, uint32_t v)
{
    const uint16_t w = (uint16_t)v;
    __builtin_memcpy(p, &w, sizeof(w));
}

static inline __attribute__((always_inline)) void px68k_native_long_store(uint8_t *p, uint32_t v)
{
    const uint16_t hi = (uint16_t)(v >> 16);
    const uint16_t lo = (uint16_t)v;
    __builtin_memcpy(p, &hi, sizeof(hi));
    __builtin_memcpy(p + 2, &lo, sizeof(lo));
}
#endif

uint32_t m68k_read_memory_8(uint32_t address)
{
#if PX68K_TAB5_FASTMEM
	address &= 0x00ffffffu;
	if (__builtin_expect(address < 0x00c00000u, 1))
		return (uint32_t)MEM[address ^ 1u];
	if (__builtin_expect(address >= 0x00fc0000u, 0))
		return (uint32_t)IPL[(address & 0x0003ffffu) ^ 1u];
#endif
	return (uint32_t) cpu_readmem24(address);
}

void m68k_write_memory_8(uint32_t address, uint32_t data)
{
#if PX68K_TAB5_FASTMEM
	address &= 0x00ffffffu;
	if (__builtin_expect(address < 0x00c00000u, 1))
	{
		BusErrFlag = 0;
		MEM[address ^ 1u] = (uint8_t)data;
		m68k_tab5_exec123_note_ram_write8(address);
		return;
	}
#endif
	cpu_writemem24(address, data);
}

uint32_t m68k_read_memory_16(uint32_t address)
{
#if PX68K_TAB5_FASTMEM
	address &= 0x00ffffffu;
	if (__builtin_expect(!(address & 1u), 1))
	{
		if (__builtin_expect(address <= 0x00bffffeu, 1))
		{
			const uint8_t *p = MEM + address;
			BusErrFlag = 0;
			return px68k_native_word_load(p);
		}
		if (__builtin_expect(address >= 0x00fc0000u && address <= 0x00fffffeu, 0))
		{
			const uint8_t *p = IPL + (address & 0x0003ffffu);
			BusErrFlag = 0;
			return px68k_native_word_load(p);
		}
	}
#endif
	return (uint32_t) cpu_readmem24_word(address);
}

void m68k_write_memory_16(uint32_t address, uint32_t data)
{
#if PX68K_TAB5_FASTMEM
	address &= 0x00ffffffu;
	if (__builtin_expect(!(address & 1u) && address <= 0x00bffffeu, 1))
	{
		uint8_t *p = MEM + address;
		BusErrFlag = 0;
		px68k_native_word_store(p, data);
		m68k_tab5_exec123_note_ram_write16(address);
		return;
	}
#endif
	cpu_writemem24_word(address, data);
}

uint32_t m68k_read_memory_32(uint32_t address)
{
#if PX68K_TAB5_FASTMEM
	address &= 0x00ffffffu;
	if (__builtin_expect(!(address & 1u), 1))
	{
		if (__builtin_expect(address <= 0x00bffffcu, 1))
		{
			const uint8_t *p = MEM + address;
			BusErrFlag = 0;
			return px68k_native_long_load(p);
		}
		if (__builtin_expect(address >= 0x00fc0000u && address <= 0x00fffffcu, 0))
		{
			const uint8_t *p = IPL + (address & 0x0003ffffu);
			BusErrFlag = 0;
			return px68k_native_long_load(p);
		}
	}
#endif
	return (uint32_t) cpu_readmem24_dword(address);
}

void IRAM_ATTR __attribute__((hot, optimize("O3"))) m68k_write_memory_32(uint32_t address, uint32_t data)
{
#if PX68K_TAB5_FASTMEM
	address &= 0x00ffffffu;
	if (__builtin_expect(!(address & 1u) && address <= 0x00bffffcu, 1))
	{
		uint8_t *p = MEM + address;
		BusErrFlag = 0;
		px68k_native_long_store(p, data);
		m68k_tab5_exec123_note_ram_write32(address);
		return;
	}
#endif
	cpu_writemem24_dword(address, data);
}

#if PX68K_TAB5_FASTMEM
#if defined(ESP_PLATFORM) && defined(__riscv)
extern void tab5_xespv_zero_byteblocks(uint8_t *dst, const uint8_t *zero_byte,
                                       uint32_t blocks16);
static int s_px68k_pie_zero_enabled = 0;
static uint32_t s_px68k_pie_zero_min_vec = 16u;

static void px68k_m68k_zero_fill_pie_selfcheck(void)
{
    enum { BASE = 0x1800, WIN = 256 };
    uint8_t backup[WIN];
    uint8_t zero = 0;
    uint8_t *base, *vec;
    uintptr_t aligned;
    unsigned i;
    int exact = 1;

    if (!MEM) return;
    base = MEM + BASE;
    memcpy(backup, base, WIN);
    memset(base, 0xA5, WIN);
    aligned = (((uintptr_t)base) + 15u) & ~(uintptr_t)15u;
    vec = (uint8_t *)aligned;
    if (vec + 128u > base + WIN) {
        memcpy(base, backup, WIN);
        return;
    }

    tab5_xespv_zero_byteblocks(vec, &zero, 8u);
    for (i = 0; i < 128u; ++i) {
        if (vec[i] != 0u) { exact = 0; break; }
    }

    /* RC: development-time C/PIE cycle A/B is retired.  The production
     * threshold measured during development is fixed at 16 bytes, guarded by
     * this exactness check and the existing scalar fallback. */
    s_px68k_pie_zero_enabled = exact ? 1 : 0;
    s_px68k_pie_zero_min_vec = exact ? 16u : 0xffffffffu;
    memcpy(base, backup, WIN);
    printf("PX68K_PIECPU599RC1: RAM ZERO exact self-check %s backend=%s minvec=%uB\n",
           exact ? "PASS" : "FAIL", exact ? "PIE-128" : "SCALAR",
           exact ? 16u : 0u);
}

#else
static void px68k_m68k_zero_fill_pie_selfcheck(void) {}
#endif

/* Build 5.98g5: side-effect-free ordinary-RAM zero-fill primitive.
 * It keeps the exact ordinary-RAM contract of 5.62, but may use the proven
 * PIE-128 zero-store primitive for a 16-byte-aligned middle segment. */
int IRAM_ATTR __attribute__((hot, optimize("O3"))) px68k_m68k_zero_fill_ram(uint32_t address, uint32_t bytes)
{
    uint8_t *dst;
    uint32_t remain;

    address &= 0x00ffffffu;
    if (!bytes || address > 0x00bfffffu || bytes > (0x00c00000u - address))
        return 0;

    dst = MEM + address;
    remain = bytes;
#if defined(ESP_PLATFORM) && defined(__riscv)
    if (__builtin_expect(s_px68k_pie_zero_enabled, 1)) {
        uint32_t pre = (uint32_t)((16u - ((uintptr_t)dst & 15u)) & 15u);
        if (pre > remain) pre = remain;
        if (pre) {
            memset(dst, 0, pre);
            dst += pre;
            remain -= pre;
        }
        if (remain >= s_px68k_pie_zero_min_vec) {
            const uint32_t blocks = remain >> 4;
            const uint32_t vec_bytes = blocks << 4;
            if (blocks) {
                const uint8_t zero = 0;
                tab5_xespv_zero_byteblocks(dst, &zero, blocks);
                dst += vec_bytes;
                remain -= vec_bytes;
            }
        }
    }
#endif
    if (remain) memset(dst, 0, remain);
    BusErrFlag = 0;
    m68k_tab5_exec123_invalidate_range(address, bytes);
    return 1;
}


/* Build 5.74: side-effect-free repeated word/long fill primitive for a
 * dynamically verified DBF store loop.  The caller has already proven that
 * all logical stores are ordinary-RAM MOVE.{W,L} Dn,(An)+ operations and
 * that code/data do not overlap.  Keep the guest's word-swapped MEM layout. */
int IRAM_ATTR __attribute__((hot, optimize("O3")))
px68k_m68k_repeat_fill_ram(uint32_t address, uint32_t value,
                           uint32_t unit_bytes, uint32_t count)
{
    uint8_t *dst;
    uint32_t bytes;

    address &= 0x00ffffffu;
    if (!count || (unit_bytes != 2u && unit_bytes != 4u) || (address & 1u))
        return 0;
    if (count > (0xffffffffu / unit_bytes)) return 0;
    bytes = count * unit_bytes;
    if (address > 0x00bfffffu || bytes > (0x00c00000u - address))
        return 0;

    dst = MEM + address;
    if (value == 0u)
    {
        memset(dst, 0, bytes);
        BusErrFlag = 0;
        m68k_tab5_exec123_invalidate_range(address, bytes);
        return 1;
    }

    if (unit_bytes == 2u)
    {
        const uint16_t w = (uint16_t)value;
        const uint32_t pair = (uint32_t)w | ((uint32_t)w << 16);
        uint32_t remain = count;
        if ((((uintptr_t)dst) & 3u) != 0u && remain)
        {
            __builtin_memcpy(dst, &w, sizeof(w));
            dst += 2;
            --remain;
        }
        while (remain >= 8u)
        {
            __builtin_memcpy(dst +  0, &pair, sizeof(pair));
            __builtin_memcpy(dst +  4, &pair, sizeof(pair));
            __builtin_memcpy(dst +  8, &pair, sizeof(pair));
            __builtin_memcpy(dst + 12, &pair, sizeof(pair));
            dst += 16;
            remain -= 8u;
        }
        while (remain >= 2u)
        {
            __builtin_memcpy(dst, &pair, sizeof(pair));
            dst += 4;
            remain -= 2u;
        }
        if (remain) __builtin_memcpy(dst, &w, sizeof(w));
    }
    else
    {
        const uint16_t hi = (uint16_t)(value >> 16);
        const uint16_t lo = (uint16_t)value;
        uint8_t pattern[4];
        uint32_t remain = count;
        __builtin_memcpy(pattern + 0, &hi, sizeof(hi));
        __builtin_memcpy(pattern + 2, &lo, sizeof(lo));
        while (remain >= 4u)
        {
            __builtin_memcpy(dst +  0, pattern, 4);
            __builtin_memcpy(dst +  4, pattern, 4);
            __builtin_memcpy(dst +  8, pattern, 4);
            __builtin_memcpy(dst + 12, pattern, 4);
            dst += 16;
            remain -= 4u;
        }
        while (remain)
        {
            __builtin_memcpy(dst, pattern, 4);
            dst += 4;
            --remain;
        }
    }

    BusErrFlag = 0;
    m68k_tab5_exec123_invalidate_range(address, bytes);
    return 1;
}

/* Build 5.70: validate and execute the live unrolled
 *
 *     MOVE.W Dn,(An)+
 *
 * stream beginning at code_address.  max_words is already capped to the
 * current Musashi timeslice by m68kcpu.c.  This function first determines the
 * exact identical-opcode prefix in ordinary guest RAM, then rejects writes
 * that could modify any opcode being elided, and only then performs the stores.
 * Returning 0 means "fall back to ordinary Musashi"; otherwise the return value
 * is the exact number of logical MOVE.W instructions completed. */
uint32_t IRAM_ATTR __attribute__((hot, optimize("O3")))
px68k_m68k_live_repeat_movew_ram(uint32_t code_address, uint32_t dest_address,
                                  uint32_t value, uint32_t opcode,
                                  uint32_t max_words)
{
    const uint16_t op16 = (uint16_t)opcode;
    const uint16_t value16 = (uint16_t)value;
    const uint32_t op_pair = (uint32_t)op16 | ((uint32_t)op16 << 16);
    const uint32_t value_pair = (uint32_t)value16 | ((uint32_t)value16 << 16);
    const uint8_t *code;
    uint8_t *dst;
    uint32_t n = 0, bytes, remain;

    code_address &= 0x00ffffffu;
    dest_address &= 0x00ffffffu;
    if (max_words < 2u || max_words > 128u ||
        (code_address & 1u) || (dest_address & 1u) ||
        code_address > 0x00bffffeu || dest_address > 0x00bffffeu)
        return 0;

    /* Never scan beyond ordinary guest RAM. */
    if (max_words > ((0x00c00000u - code_address) >> 1))
        max_words = (0x00c00000u - code_address) >> 1;
    if (max_words < 2u) return 0;

    code = MEM + code_address;

    /* Count the verified identical-opcode prefix. Pair loads halve host loads
     * on the common aligned case, while memcpy avoids unaligned UB. */
    if ((((uintptr_t)code) & 3u) != 0u && n < max_words)
    {
        uint16_t got;
        __builtin_memcpy(&got, code, sizeof(got));
        if (got != op16) return 0;
        code += 2;
        ++n;
    }
    while (n + 2u <= max_words)
    {
        uint32_t got;
        __builtin_memcpy(&got, code, sizeof(got));
        if (got == op_pair)
        {
            code += 4;
            n += 2u;
            continue;
        }
        {
            uint16_t got16;
            __builtin_memcpy(&got16, code, sizeof(got16));
            if (got16 == op16) ++n;
        }
        break;
    }
    if (n < max_words && n + 1u == max_words)
    {
        uint16_t got;
        __builtin_memcpy(&got, MEM + code_address + (n << 1), sizeof(got));
        if (got == op16) ++n;
    }
    if (n < 2u) return 0;

    bytes = n << 1;
    if (bytes > (0x00c00000u - dest_address))
        return 0;

    /* If any store could rewrite an instruction this very batch is about to
     * elide, sequential 68000 execution could observe that changed opcode. */
    if (!(dest_address + bytes <= code_address ||
          dest_address >= code_address + bytes))
        return 0;

    dst = MEM + dest_address;
    remain = n;

    if ((((uintptr_t)dst) & 3u) != 0u)
    {
        __builtin_memcpy(dst, &value16, sizeof(value16));
        dst += 2;
        --remain;
    }
    while (remain >= 8u)
    {
        __builtin_memcpy(dst +  0, &value_pair, sizeof(value_pair));
        __builtin_memcpy(dst +  4, &value_pair, sizeof(value_pair));
        __builtin_memcpy(dst +  8, &value_pair, sizeof(value_pair));
        __builtin_memcpy(dst + 12, &value_pair, sizeof(value_pair));
        dst += 16;
        remain -= 8u;
    }
    while (remain >= 2u)
    {
        __builtin_memcpy(dst, &value_pair, sizeof(value_pair));
        dst += 4;
        remain -= 2u;
    }
    if (remain)
        __builtin_memcpy(dst, &value16, sizeof(value16));

    BusErrFlag = 0;
    m68k_tab5_exec123_invalidate_range(dest_address, bytes);
    return n;
}

/* Build 5.54: dedicated opcode/immediate path from Musashi m68ki_read_imm_*().
 * ADDRESS_68K() has already masked the address before these functions are
 * called, so RAM fetch is one range test plus a native halfword load. */
uint32_t IRAM_ATTR __attribute__((hot, optimize("O3"))) px68k_m68k_fetch_16(uint32_t address)
{
    if (__builtin_expect(address <= 0x00bffffeu, 1))
    {
        BusErrFlag = 0;
        return px68k_native_word_load(MEM + address);
    }
    if (__builtin_expect(address >= 0x00fc0000u && address <= 0x00fffffeu, 0))
    {
        BusErrFlag = 0;
        return px68k_native_word_load(IPL + (address & 0x0003ffffu));
    }
    return (uint32_t)cpu_readmem24_word(address);
}

uint32_t IRAM_ATTR __attribute__((hot, optimize("O3"))) px68k_m68k_fetch_32(uint32_t address)
{
    if (__builtin_expect(address <= 0x00bffffcu, 1))
    {
        BusErrFlag = 0;
        return px68k_native_long_load(MEM + address);
    }
    if (__builtin_expect(address >= 0x00fc0000u && address <= 0x00fffffcu, 0))
    {
        BusErrFlag = 0;
        return px68k_native_long_load(IPL + (address & 0x0003ffffu));
    }
    return (uint32_t)cpu_readmem24_dword(address);
}
#endif

#endif /* HAVE_CYCLONE */ /* HAVE_MUSASHI */


/******************************************************************************
	M68000���󥿥ե������ؿ�E
******************************************************************************/

/*--------------------------------------------------------
	CPU�鴁E�
--------------------------------------------------------*/
#if !defined (HAVE_MUSASHI)
int32_t my_irqh_callback(int32_t level);
#endif

void m68000_init(void)
{
#if defined (HAVE_CYCLONE)

	m68k.read8  = read8;
	m68k.read16 = read16;
	m68k.read32 = cpu_readmem24_dword;

	m68k.fetch8  = read8;
	m68k.fetch16 = read16;
	m68k.fetch32 = cpu_readmem24_dword;

	m68k.write8  = cpu_writemem24;
	m68k.write16 = cpu_writemem24_word;
	m68k.write32 = cpu_writemem24_dword;

	m68k.checkpc = MyCheckPc;

	m68k.IrqCallback = my_irqh_callback;

	CycloneInit();

#elif defined (HAVE_C68K)
    C68k_Init(&C68K, my_irqh_callback);
    C68k_Set_ReadB(&C68K, cpu_readmem24);
    C68k_Set_ReadW(&C68K, cpu_readmem24_word);
    C68k_Set_WriteB(&C68K, cpu_writemem24);
    C68k_Set_WriteW(&C68K, cpu_writemem24_word);
	C68k_Set_Fetch(&C68K, 0x000000, 0xbfffff, (uintptr_t)MEM);
    C68k_Set_Fetch(&C68K, 0xc00000, 0xc7ffff, (uintptr_t)GVRAM);
    C68k_Set_Fetch(&C68K, 0xe00000, 0xe7ffff, (uintptr_t)TVRAM);
    C68k_Set_Fetch(&C68K, 0xea0000, 0xea1fff, (uintptr_t)SCSIIPL);
    C68k_Set_Fetch(&C68K, 0xed0000, 0xed3fff, (uintptr_t)SRAM);
    C68k_Set_Fetch(&C68K, 0xf00000, 0xfbffff, (uintptr_t)FONT);
    C68k_Set_Fetch(&C68K, 0xfc0000, 0xffffff, (uintptr_t)IPL);
#elif defined (HAVE_MUSASHI)
    m68k_set_cpu_type(M68K_CPU_TYPE_68000);
    m68k_init();
#ifdef ESP_PLATFORM
#if PX68K_TAB5_FASTMEM
    px68k_m68k_zero_fill_pie_selfcheck();
#endif
    px68k_p4_isa_banner();
    printf("PX68K_CPU: Build 5.62 FDC+GPIP/fill fast-forward + host arithmetic sweep + DISPATCH256 enabled\n");
#endif
    px68k_illegal_count = 0;
    m68k_set_illg_instr_callback(px68k_illegal_diag);
#endif /* HAVE_C68K */ /* HAVE_MUSASHI */
}


/*--------------------------------------------------------
	CPU��E��å�
--------------------------------------------------------*/

void m68000_reset(void)
{
#if defined (HAVE_CYCLONE)
	CycloneReset(&m68k);
	m68k.state_flags = 0; /* Go to default state (not stopped, halted, etc.) */
	m68k.srh = 0x27; /* Set supervisor mode */
#elif defined (HAVE_C68K)
	C68k_Reset(&C68K);
#elif defined (HAVE_MUSASHI)
	m68k_pulse_reset();
#endif /* HAVE_C68K */ /* HAVE_MUSASHI */
}


/*--------------------------------------------------------
	CPU���
--------------------------------------------------------*/

void m68000_exit(void)
{
}


/*--------------------------------------------------------
	CPU�¹�
--------------------------------------------------------*/

int m68000_execute(int cycles)
{	
#if defined (HAVE_CYCLONE)
	m68k.cycles = cycles;
	CycloneRun(&m68k);
	return m68k.cycles ;
#elif defined (HAVE_C68K)
	return C68k_Exec(&C68K, cycles);
#elif defined (HAVE_MUSASHI)
        return m68k_execute(cycles);
#endif /* HAVE_C68K */ /* HAVE_MUSASHI */
}



/*--------------------------------------------------------
	�䤁E��߽���
--------------------------------------------------------*/

void m68000_set_irq_line(int irqline, int state)
{
#if defined (HAVE_CYCLONE)
	m68k.irq = irqline;
#elif defined (HAVE_C68K)
	C68k_Set_IRQ(&C68K, irqline);
#elif defined (HAVE_MUSASHI)
	m68k_set_irq(irqline);
#endif /* HAVE_C68K */ /* HAVE_MUSASHI */
}

/*--------------------------------------------------------
	��E���������
--------------------------------------------------------*/

uint32_t m68000_get_reg(int regnum)
{
#if defined (HAVE_CYCLONE)
	switch (regnum)
	{
		case M68K_PC: return m68k.pc - m68k.membase;
		case M68K_SR: return CycloneGetSr(&m68k);
		case M68K_D0: return m68k.d[0];
		case M68K_D1: return m68k.d[1];
		case M68K_D2: return m68k.d[2];
		case M68K_D3: return m68k.d[3];
		case M68K_D4: return m68k.d[4];
		case M68K_D5: return m68k.d[5];
		case M68K_D6: return m68k.d[6];
		case M68K_D7: return m68k.d[7];
		case M68K_A0: return m68k.a[0];
		case M68K_A1: return m68k.a[1];
		case M68K_A2: return m68k.a[2];
		case M68K_A3: return m68k.a[3];
		case M68K_A4: return m68k.a[4];
		case M68K_A5: return m68k.a[5];
		case M68K_A6: return m68k.a[6];
		case M68K_A7: return m68k.a[7];
		default:
			break;
	}
	return 0x0BADC0DE;
#elif defined (HAVE_C68K)
	switch (regnum)
	{
	case M68K_PC:  return C68k_Get_PC(&C68K);
	case M68K_USP: return C68k_Get_USP(&C68K);
	case M68K_MSP: return C68k_Get_MSP(&C68K);
	case M68K_SR:  return C68k_Get_SR(&C68K);
	case M68K_D0:  return C68k_Get_DReg(&C68K, 0);
	case M68K_D1:  return C68k_Get_DReg(&C68K, 1);
	case M68K_D2:  return C68k_Get_DReg(&C68K, 2);
	case M68K_D3:  return C68k_Get_DReg(&C68K, 3);
	case M68K_D4:  return C68k_Get_DReg(&C68K, 4);
	case M68K_D5:  return C68k_Get_DReg(&C68K, 5);
	case M68K_D6:  return C68k_Get_DReg(&C68K, 6);
	case M68K_D7:  return C68k_Get_DReg(&C68K, 7);
	case M68K_A0:  return C68k_Get_AReg(&C68K, 0);
	case M68K_A1:  return C68k_Get_AReg(&C68K, 1);
	case M68K_A2:  return C68k_Get_AReg(&C68K, 2);
	case M68K_A3:  return C68k_Get_AReg(&C68K, 3);
	case M68K_A4:  return C68k_Get_AReg(&C68K, 4);
	case M68K_A5:  return C68k_Get_AReg(&C68K, 5);
	case M68K_A6:  return C68k_Get_AReg(&C68K, 6);
	case M68K_A7:  return C68k_Get_AReg(&C68K, 7);

	default: return 0;
	}
#elif defined (HAVE_MUSASHI)
	switch (regnum)
	{
	case M68K_PC:  return m68k_get_reg(NULL, M68K_REG_PC);
	case M68K_USP: return m68k_get_reg(NULL, M68K_REG_USP);
	case M68K_MSP: return m68k_get_reg(NULL, M68K_REG_MSP);
	case M68K_SR:  return m68k_get_reg(NULL, M68K_REG_SR);
	case M68K_D0:  return m68k_get_reg(NULL, M68K_REG_D0);
	case M68K_D1:  return m68k_get_reg(NULL, M68K_REG_D1);
	case M68K_D2:  return m68k_get_reg(NULL, M68K_REG_D2);
	case M68K_D3:  return m68k_get_reg(NULL, M68K_REG_D3);
	case M68K_D4:  return m68k_get_reg(NULL, M68K_REG_D4);
	case M68K_D5:  return m68k_get_reg(NULL, M68K_REG_D5);
	case M68K_D6:  return m68k_get_reg(NULL, M68K_REG_D6);
	case M68K_D7:  return m68k_get_reg(NULL, M68K_REG_D7);
	case M68K_A0:  return m68k_get_reg(NULL, M68K_REG_A0);
	case M68K_A1:  return m68k_get_reg(NULL, M68K_REG_A1);
	case M68K_A2:  return m68k_get_reg(NULL, M68K_REG_A2);
	case M68K_A3:  return m68k_get_reg(NULL, M68K_REG_A3);
	case M68K_A4:  return m68k_get_reg(NULL, M68K_REG_A4);
	case M68K_A5:  return m68k_get_reg(NULL, M68K_REG_A5);
	case M68K_A6:  return m68k_get_reg(NULL, M68K_REG_A6);
	case M68K_A7:  return m68k_get_reg(NULL, M68K_REG_A7);

	default: return 0;
	}
#endif /* HAVE_C68K */ /* HAVE_MUSASHI */
}


/*--------------------------------------------------------
	��E�������āE
--------------------------------------------------------*/

void m68000_set_reg(int regnum, uint32_t val)
{
#if defined (HAVE_CYCLONE)
	switch (regnum)
	{
		case M68K_PC:
		  	m68k.pc = m68k.checkpc(val+m68k.membase);
			break;
		case M68K_SR:
		  	CycloneSetSr(&m68k, val);
			break;
		case M68K_D0: m68k.d[0] = val; break;
		case M68K_D1: m68k.d[1] = val; break;
		case M68K_D2: m68k.d[2] = val; break;
		case M68K_D3: m68k.d[3] = val; break;
		case M68K_D4: m68k.d[4] = val; break;
		case M68K_D5: m68k.d[5] = val; break;
		case M68K_D6: m68k.d[6] = val; break;
		case M68K_D7: m68k.d[7] = val; break;
		case M68K_A0: m68k.a[0] = val; break;
		case M68K_A1: m68k.a[1] = val; break;
		case M68K_A2: m68k.a[2] = val; break;
		case M68K_A3: m68k.a[3] = val; break;
		case M68K_A4: m68k.a[4] = val; break;
		case M68K_A5: m68k.a[5] = val; break;
		case M68K_A6: m68k.a[6] = val; break;
		case M68K_A7: m68k.a[7] = val; break;

		
		default: break;
	}	
#elif defined (HAVE_C68K)
	switch (regnum)
	{
	case M68K_PC:  C68k_Set_PC(&C68K, val); break;
	case M68K_USP: C68k_Set_USP(&C68K, val); break;
	case M68K_MSP: C68k_Set_MSP(&C68K, val); break;
	case M68K_SR:  C68k_Set_SR(&C68K, val); break;
	case M68K_D0:  C68k_Set_DReg(&C68K, 0, val); break;
	case M68K_D1:  C68k_Set_DReg(&C68K, 1, val); break;
	case M68K_D2:  C68k_Set_DReg(&C68K, 2, val); break;
	case M68K_D3:  C68k_Set_DReg(&C68K, 3, val); break;
	case M68K_D4:  C68k_Set_DReg(&C68K, 4, val); break;
	case M68K_D5:  C68k_Set_DReg(&C68K, 5, val); break;
	case M68K_D6:  C68k_Set_DReg(&C68K, 6, val); break;
	case M68K_D7:  C68k_Set_DReg(&C68K, 7, val); break;
	case M68K_A0:  C68k_Set_AReg(&C68K, 0, val); break;
	case M68K_A1:  C68k_Set_AReg(&C68K, 1, val); break;
	case M68K_A2:  C68k_Set_AReg(&C68K, 2, val); break;
	case M68K_A3:  C68k_Set_AReg(&C68K, 3, val); break;
	case M68K_A4:  C68k_Set_AReg(&C68K, 4, val); break;
	case M68K_A5:  C68k_Set_AReg(&C68K, 5, val); break;
	case M68K_A6:  C68k_Set_AReg(&C68K, 6, val); break;
	case M68K_A7:  C68k_Set_AReg(&C68K, 7, val); break;
	default: break;
	}
#elif defined (HAVE_MUSASHI)
	switch (regnum)
	{
	case M68K_PC:  m68k_set_reg(M68K_REG_PC, val); break;
	case M68K_USP: m68k_set_reg(M68K_REG_USP, val); break;
	case M68K_MSP: m68k_set_reg(M68K_REG_MSP, val); break;
	case M68K_SR:  m68k_set_reg(M68K_REG_SR, val); break;
	case M68K_D0:  m68k_set_reg(M68K_REG_D0, val); break;
	case M68K_D1:  m68k_set_reg(M68K_REG_D1, val); break;
	case M68K_D2:  m68k_set_reg(M68K_REG_D2, val); break;
	case M68K_D3:  m68k_set_reg(M68K_REG_D3, val); break;
	case M68K_D4:  m68k_set_reg(M68K_REG_D4, val); break;
	case M68K_D5:  m68k_set_reg(M68K_REG_D5, val); break;
	case M68K_D6:  m68k_set_reg(M68K_REG_D6, val); break;
	case M68K_D7:  m68k_set_reg(M68K_REG_D7, val); break;
	case M68K_A0:  m68k_set_reg(M68K_REG_A0, val); break;
	case M68K_A1:  m68k_set_reg(M68K_REG_A1, val); break;
	case M68K_A2:  m68k_set_reg(M68K_REG_A2, val); break;
	case M68K_A3:  m68k_set_reg(M68K_REG_A3, val); break;
	case M68K_A4:  m68k_set_reg(M68K_REG_A4, val); break;
	case M68K_A5:  m68k_set_reg(M68K_REG_A5, val); break;
	case M68K_A6:  m68k_set_reg(M68K_REG_A6, val); break;
	case M68K_A7:  m68k_set_reg(M68K_REG_A7, val); break;
	default: break;
	}

#endif /* HAVE_C68K */ /* HAVE_MUSASHI */
}


/*------------------------------------------------------
	������/������ ���ơ���
------------------------------------------------------*/

#ifdef SAVE_STATE

STATE_SAVE( m68000 )
{
#if defined (HAVE_CYCLONE)
/* empty */
#elif defined (HAVE_C68K)
	int i;
	uint32_t pc = C68k_Get_Reg(&C68K, C68K_PC);

	for (i = 0; i < 8; i++)
		state_save_long(&C68K.D[i], 1);
	for (i = 0; i < 8; i++)
		state_save_long(&C68K.A[i], 1);

	state_save_long(&C68K.flag_C, 1);
	state_save_long(&C68K.flag_V, 1);
	state_save_long(&C68K.flag_Z, 1);
	state_save_long(&C68K.flag_N, 1);
	state_save_long(&C68K.flag_X, 1);
	state_save_long(&C68K.flag_I, 1);
	state_save_long(&C68K.flag_S, 1);
	state_save_long(&C68K.USP, 1);
	state_save_long(&pc, 1);
	state_save_long(&C68K.HaltState, 1);
	state_save_long(&C68K.IRQLine, 1);
	state_save_long(&C68K.IRQState, 1);
#endif /* HAVE_C68K */
}

STATE_LOAD( m68000 )
{
#if defined (HAVE_CYCLONE)
/* empty */
#elif defined (HAVE_C68K)
	int i;
	uint32_t pc;

	for (i = 0; i < 8; i++)
		state_load_long(&C68K.D[i], 1);
	for (i = 0; i < 8; i++)
		state_load_long(&C68K.A[i], 1);

	state_load_long(&C68K.flag_C, 1);
	state_load_long(&C68K.flag_V, 1);
	state_load_long(&C68K.flag_Z, 1);
	state_load_long(&C68K.flag_N, 1);
	state_load_long(&C68K.flag_X, 1);
	state_load_long(&C68K.flag_I, 1);
	state_load_long(&C68K.flag_S, 1);
	state_load_long(&C68K.USP, 1);
	state_load_long(&pc, 1);
	state_load_long(&C68K.HaltState, 1);
	state_load_long(&C68K.IRQLine, 1);
	state_load_long(&C68K.IRQState, 1);

	C68k_Set_Reg(&C68K, C68K_PC, pc);
#endif /* HAVE_C68K */
}

#endif /* SAVE_STATE */
