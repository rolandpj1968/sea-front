// EXPECT: 0
// Free function templates with literal NTTP args must mangle
// distinctly per arg value. Previously the cloned-func name
// builder used type_arg_from_node for each template arg —
// which only recognised ND_VAR_DECL and dropped literal args,
// emitting `unknown` for every NTTP. Result: identity<10> and
// identity<20> both mangled `identity_t_unknown_te__p__pe_` and
// collided at link time; the second clone overwrote the first
// and `identity<10>()` returned 20.
//
// Same bug applied to function-pointer NTTPs (call_with<&square>
// vs call_with<&cube>) — both collapsed to the same `unknown`
// mangle.
//
// Fix: use template_arg_to_arg_type (which handles literals + &fn
// + ident shapes via TY_NTTP_VALUE) and propagate the param's
// declared type as nttp_decl_type so the mangler renders
// `int_10` / `int_20`. Function-pointer NTTPs render the bound
// token text (e.g. `square` / `cube`).
//
// N4659 §17.7.1 [temp.inst]: each distinct specialization is one
// entity. Sea-front's mangle must discriminate them.

extern "C" void abort();

template<int N> int identity() { return N; }

int square(int x) { return x * x; }
int cube(int x)   { return x * x * x; }

template<int (*F)(int)> int call_with(int v) { return F(v); }

int main() {
    if (identity<10>() != 10) abort();
    if (identity<20>() != 20) abort();
    if (identity<10>() != 10) abort();  /* re-call should hit dedup */

    if (call_with<&square>(5) != 25) abort();
    if (call_with<&cube>(3) != 27) abort();
    return 0;
}
