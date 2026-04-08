// Additional base-class lookup coverage — N4659 §6.4.2 [class.member.lookup]
// and §6.3.10 [basic.scope.hiding]. Each case targets a guarantee
// that base_lookup.cpp / base_edge.cpp / base_lookup_proper.cpp don't
// individually pin down.

// 1. Qualified inherited call: 'Base::method()' from a derived
//    method body. Exercises the qualified-name path through the
//    inheritance chain.
struct B1 { void m() {} };
struct D1 : B1 {
    void f() { B1::m(); }
};

// 2. Hidden by derived — N4659 §6.3.10/1 [basic.scope.hiding]:
//    a derived-class declaration with the same name as an
//    inherited one HIDES the inherited declaration in the
//    derived class's scope. Lookup must find the derived 'T'
//    (long) before reaching the base 'T' (int).
struct B2 { typedef int T; };
struct D2 : B2 {
    typedef long T;
    void f() { T x = 0; (void)x; }    // resolves to long, not int
};

// 3. Private inheritance — we don't model access control, but
//    the parser must accept the keyword and base lookup must
//    still find inherited members (the access check happens
//    in sema, which we don't do for this yet).
struct B3 {
    typedef int T;
    void m() {}
};
struct D3 : private B3 {
    void f() {
        T x = 0;
        m();
        (void)x;
    }
};

// 4. Distinct-declaration diamond — both A4 and B4 declare 'm()';
//    standard says reaching them via D4 is ambiguous. We don't
//    diagnose this yet (first match wins silently). The test just
//    verifies that the parser doesn't choke on the structure and
//    that NOT calling 'm()' from D4's body still parses fine.
struct A4 { void m() {} };
struct B4 { void m() {} };
struct D4 : A4, B4 {
    void f() {
        // Calling 'm()' here would be ambiguous per the standard.
        // We don't yet diagnose; documented as a known limitation.
        A4::m();   // qualified — unambiguous
        B4::m();   // qualified — unambiguous
    }
};

// 5. Three-level chain with hiding in the middle — Leaf should
//    see Mid::v (long), not Base::v (int).
struct Base5 { typedef int v; };
struct Mid5  : Base5 { typedef long v; };
struct Leaf5 : Mid5 {
    void f() { v x = 0; (void)x; }   // resolves to long
};
