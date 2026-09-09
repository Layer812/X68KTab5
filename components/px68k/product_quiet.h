#pragma once
/* P12R1 HF1 Production Quiet.
 * C/C++: suppress PX68K host diagnostic printf calls without evaluating
 * arguments and without triggering -Wunused-value under -Werror.
 * ASM: forced -include reaches preprocessed .S sources too, so expose no C
 * headers or macros to the assembler preprocessor. */
#ifndef __ASSEMBLER__
#include <stdio.h>
#ifdef printf
#undef printf
#endif
#define printf(...) ((void)0)
#endif
