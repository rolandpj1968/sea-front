// EXPECT: 0
// `extern template class S<char>;` is an explicit-instantiation
// DECLARATION per N4659 §17.7.2 [temp.explicit] — it promises that
// some other TU will provide the instantiation. The parser produces
// an ND_TEMPLATE_DECL with nparams=0 wrapping a bodyless, nameless
// ND_VAR_DECL whose ty is the template-id.
//
// registry_find_specialization used to treat this as a full
// specialization, returning the nameless var-decl as `inst`. The
// dedup_add for class instantiations was gated on
// `inst->kind == ND_CLASS_DEF`, so the explicit-inst-decl never
// reached the dedup set. Every subsequent request for the same
// template-id then re-resolved the explicit-inst-decl and got a
// fresh nameless var-decl appended to all_instantiated[]. The
// worklist loop's next pass walked those, found the same template-id
// references inside the body of the (still-uninstantiated) primary
// class, and looped indefinitely (hang in libstdc++ headers
// alone — e.g. `<string>` declares `extern template class
// basic_string<char>;`).
//
// Fix: skip explicit-inst-decls in registry_find_specialization.

extern "C" void abort();

template<typename T> struct S {
    int v;
    S() : v(7) {}
    int get() const { return v; }
};

extern template class S<char>;   /* the trigger */

int main() {
    S<char> s;
    if (s.get() != 7) abort();
    return 0;
}
