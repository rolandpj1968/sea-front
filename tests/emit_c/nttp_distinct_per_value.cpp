// EXPECT: 1
// Two NTTP instantiations of the same template with DIFFERENT values
// must yield distinct types — and distinct mangled call-site symbols.
//
// Surfaced by gcc 14's libcpp build under Itanium mangling: every
// `integral_constant<bool, V>` instantiation collapsed to a single
// canonical Type because canonicalize_type's dedup key handled only
// ND_VAR_DECL template args, not literal NTTP nodes. Result: all
// `IbLb0EE` and `IbLb1EE` symbol references pointed to whichever
// canonical instance won, so the cv operator returned the same
// value regardless of V.

template <typename T, T V>
struct ic {
    T get() const { return V; }
};

int main() {
    ic<bool, true>  t;
    ic<bool, false> f;
    return t.get() - f.get();   // 1 - 0 = 1
}
