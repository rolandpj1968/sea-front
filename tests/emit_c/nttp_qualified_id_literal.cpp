// EXPECT: 42
// Explicit NTTP literal accessed via Class<args>::member — the
// qualified-id stub builder in emit_c.c must extract a TY_NTTP_VALUE
// from non-ND_VAR_DECL template-argument shapes (ND_NUM, ND_BOOL_LIT,
// ...) so the mangled tag at the use site matches the definition.
//
// Without that, 'M<int, 7>::trait' emits as
// 'sf__M_t_int_unknown_te___trait' while the instantiated definition
// is 'sf__M_t_int_int_7_te___trait', and the link fails.
//
// N4659 §17.1/4 [temp.param] + §17.7.1 [temp.inst].

template<typename T, int v>
struct M { static const int trait = v; };

int r = M<int, 42>::trait;

int main() { return r; }
