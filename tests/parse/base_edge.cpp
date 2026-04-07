// Edge cases around base-class chain lookup.

// 1. Templated base — the base type is opaque (no class_region),
//    so the chain falls back to the heuristic. Parser should not
//    crash and Derived's body should still parse.
template<typename T> struct Base { typedef T value_type; };

template<typename T> struct Derived : Base<T> {
    // 'value_type' here resolves through the heuristic, not the
    // base-class chain — Base<T> is an opaque template-id at parse
    // time. The IDENT-IDENT shape is what saves us.
    void f() {
        value_type x = 0;
        (void)x;
    }
};

// 2. Out-of-class method body — the method is defined OUTSIDE the
//    class, so we're at namespace scope when the body is parsed.
//    The base-class chain isn't reachable from here either; the
//    inherited typedef must be resolved by the heuristic.
struct B { typedef int size_type; };
struct D : B { void g(); };
void D::g() {
    size_type n = 7;
    (void)n;
}

// 3. Diamond inheritance — depth-first first-match. Documents that
//    we don't yet diagnose ambiguous lookup; the test passes as long
//    as 'tag' is recognised as a type-name from one of the bases.
struct Top { typedef long tag; };
struct Mid1 : Top { };
struct Mid2 : Top { };
struct Bot : Mid1, Mid2 {
    void h() {
        tag t = 0;
        (void)t;
    }
};
