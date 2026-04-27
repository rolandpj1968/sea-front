// EXPECT: 0
// N4659 §5.13.5/13 [lex.string]: adjacent string-literal tokens
// concatenate in translation phase 6. C99 §6.4.5/4 has the same
// rule. Sea-front previously CONSUMED adjacent string tokens at
// parse time but only stored the first; emit_str produced just the
// first token's content. Result: 'puts("a" "b" "c")' became
// 'puts("a")' — silently dropping 'b' and 'c'.
//
// Surfaced by gcc 4.8 build-tools' genmodes.c which uses
//   #define print_decl(TYPE, NAME, ASIZE) \
//       puts("\nconst " TYPE " " NAME "[" ASIZE "] =\n{");
// to print 'const char *const mode_name[NUM_MACHINE_MODES] = {'.
// With sea-front dropping the adjacent strings, genmodes' output
// was a useless 'const ' line, and downstream min-insn-modes.o
// failed to build.
//
// Sea-front now records (first-token, count) and emits all tokens
// with whitespace between — the C compiler concatenates them at
// its parse time per §6.4.5/4.
#include <stdio.h>

int main() {
    const char *s = "abc" "def" "ghi";
    if (s[0] != 'a' || s[2] != 'c' || s[3] != 'd' || s[8] != 'i') return 1;
    return 0;
}
