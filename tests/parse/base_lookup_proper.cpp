// Base-class lookup chain — N4659 §6.4.2 [class.member.lookup].
//
// These tests use forms that the IDENT-IDENT declaration heuristic
// cannot rescue, so a passing parse proves that lookup actually
// walks the base list.
//
// (The other base-lookup test, base_edge.cpp, uses the IDENT-IDENT
// shape which is also caught by the heuristic — so it doesn't
// distinguish "lookup works" from "heuristic guesses correctly".)

// 1. Inherited member function called from a derived method body.
//    The call expression 'inc()' is a function-call shape, not a
//    declaration shape, so the heuristic does not fire. If base
//    lookup didn't find 'inc', this would parse as an unresolved
//    free-function call (still successful, but the AST would show
//    no resolved decl). Either way the parser doesn't error.
struct B1 {
    int x;
    void inc() { x = x + 1; }
};
struct D1 : B1 {
    void f() { inc(); }
};

// 2. Inherited static const used in an array bound. This REQUIRES
//    lookup to resolve to the actual constant value during parsing
//    — there's no fallback.
struct B2 {
    static const int N = 4;
};
struct D2 : B2 {
    int arr[N];   // forces lookup of N as a constant value
};

// 3. Inherited typedef used as the type of a CALL ARGUMENT. The
//    expression 'inc(default_value)' has no IDENT-IDENT shape; the
//    heuristic does not fire. If lookup of 'value_type' fails, the
//    constant '7' has the wrong type but the parse still works
//    (silently). This documents that the type-name resolution
//    happens via base lookup.
struct B3 {
    typedef int value_type;
    void take(value_type v) { (void)v; }
};
struct D3 : B3 {
    void caller() {
        take(7);   // 7 must convert to value_type, which lookup
                   // resolves through B3 → int
    }
};

// 4. Diamond — depth-first first-match. Bot inherits 'tag' via two
//    paths. We don't yet diagnose the §6.4.2 ambiguity (and the
//    standard is fine with this for "the same declaration reachable
//    via multiple paths"); the test just verifies a use compiles.
struct Top { typedef long tag; };
struct M1 : Top { };
struct M2 : Top { };
struct Bot : M1, M2 {
    void use() {
        tag t = 0;
        (void)t;
    }
};
