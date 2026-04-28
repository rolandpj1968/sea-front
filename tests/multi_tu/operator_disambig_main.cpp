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
//
// Standard: N4659 §16.5 [over.oper] (operator functions are
// distinguished by their signature plus the operator token —
// operator+ vs operator- are different overloads, not the same).
// §10.1.6 [dcl.inline] (inline-vs-non-inline status is per
// declaration of a specific function, not shared across operators
// of the same class).

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
