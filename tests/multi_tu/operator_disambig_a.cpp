// Half of the operator-disambiguation multi-TU test. This file
// has the class definition AND the OOL non-inline body for
// operator+. The other file (operator_disambig_main.cpp) has
// only the class definition (no OOL bodies — sea-front would
// otherwise multi-def-link).
//
// Without the disambiguation fix (4952014):
// find_ool_method_storage matched the FIRST OOL whose class
// tag + name + arity match. For class operators (all named
// 'operator'), the first inline OOL operator's DECL_INLINE
// leaked onto every non-inline OOL operator's in-class
// fwd-decl emit, narrowing the def's linkage to file-static.
// Cross-TU callers (in main.cpp) couldn't link.

struct Box {
    int v;
    Box operator + (Box b) const;          // OOL non-inline (this TU)
    inline Box operator - (Box b) const;   // OOL inline (this TU)
};

Box Box::operator + (Box b) const {
    Box r; r.v = v + b.v; return r;
}

inline Box Box::operator - (Box b) const {
    Box r; r.v = v - b.v; return r;
}

int run_a(int x, int y) {
    Box a; a.v = x;
    Box b; b.v = y;
    return (a + b).v;
}
