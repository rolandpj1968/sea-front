// EXPECT: 10
// '::name(args)' must lower as a real call, not collapse into a
// comment-shaped placeholder + comma-expression on its args.
//
// The parser used to build ND_QUALIFIED for any leading-:: name even
// when it had only one part, and emit_expr had no handler for
// ND_QUALIFIED — so it printed `/* expr */` as the callee. The
// surrounding C stayed parseable: `int r = /* expr */(5);` becomes
// `int r = (5);`. Caught at runtime in gcc 4.8's va_gc::reserve where
// `v = ::ggc_realloc_stat(v, size);` reduced to `*v = (vec*)size;`,
// producing a fake pointer like 0x28 that crashed embedded_init.
//
// N4659 §8.1.4.3 [expr.prim.id.qual]: '::name' is just lookup in the
// global namespace, ignoring local shadows. For TU-scope free
// functions that's the same as 'name'.
extern "C" int printf(const char*, ...);
int g_func(int x) { return x * 2; }

int main() {
    return ::g_func(5);  // 10
}
