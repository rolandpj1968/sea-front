// EXPECT: 7
// Companion to operator_disambig_a.cpp. This TU only sees the
// class declaration (no OOL bodies, to avoid multi-def link).
// Calls run_a() which exercises the cross-TU operator+ link.
//
// Without 4952014 (operator disambiguation by full sig + op token):
// find_ool_method_storage in this TU's class declaration would
// match the FIRST OOL whose tag+name+arity match. For class
// operators (all named 'operator'), the first inline OOL operator
// would leak its DECL_INLINE onto the non-inline operator's
// fwd-decl emit, narrowing linkage to file-static. This TU's
// caller of (a+b) couldn't link to the other TU's def.

struct Box {
    int v;
    Box operator + (Box b) const;          // OOL non-inline (defined in _a.cpp)
    inline Box operator - (Box b) const;   // OOL inline
};

inline Box Box::operator - (Box b) const {
    Box r; r.v = v - b.v; return r;
}

extern int run_a(int x, int y);

int main() {
    return run_a(3, 4);  // 3 + 4 = 7
}
