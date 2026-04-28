// Half of the gnu_inline-vs-strong-def dedup multi-TU test.
// This TU has BOTH forms of f() — exactly as gcc 4.8 tree.c
// sees after preprocessing tree.h:
//   - 'extern inline' (GNU gnu_inline semantics: body is a HINT,
//     no symbol emitted from this TU)
//   - the strong out-of-line def (provides the exported symbol)
//
// Without 248271c: func_def_dedup_check_sig recorded the
// gnu_inline def first, then dropped the later strong def as a
// duplicate. Result: this TU's .o exported only the gnu_inline
// hint (no body) — 'U f' instead of 'T f'. Cross-TU callers
// failed to link with hundreds of unresolved refs (211 in
// cc1plus alone for tree_low_cst).
//
// Fix: track is_gnu_inline_def per FuncSig. Strong def following
// a gnu_inline is NOT a duplicate; emit both. C99 §6.2.2/4
// + GNU 'extern inline' semantics.

extern inline __attribute__((__gnu_inline__))
int f(int x) { return x + 1; }   // hint, no symbol from this TU

int f(int x) { return x + 1; }    // strong OOL — must reach the .o
