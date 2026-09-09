#ifndef M68KOPS__HEADER
#define M68KOPS__HEADER

/* ======================================================================== */
/* ============================ OPCODE HANDLERS =========================== */
/* ======================================================================== */


/* Build the opcode handler table */
void m68ki_build_opcode_table(void);

extern void (*m68ki_instruction_jump_table[0x10000])(void); /* opcode handler jump table */
extern unsigned char m68ki_cycles[][0x10000];


#ifdef ESP_PLATFORM
/* R140M2 exact-PC MDX fused blocks. */
unsigned int m68k_tab5_mdxm2_exec(unsigned int pc, unsigned int op);
void m68k_tab5_mdxm2_maybe_report(void);
#endif

/* ======================================================================== */
/* ============================== END OF FILE ============================= */
/* ======================================================================== */

#endif /* M68KOPS__HEADER */


