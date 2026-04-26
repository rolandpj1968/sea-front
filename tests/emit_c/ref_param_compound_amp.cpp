// EXPECT: 7
// N4659 §11.3.2 [dcl.ref] — reference params lower to pointers; uses
// must be deref'd. The previous narrow rule "TK_AMP suppresses
// ref-deref" fired on '&v->x' too, leaking the suppression into the
// nested ident emission and emitting bare 'v->x' (a pointer-to-pointer
// arrow access) where '(*v)->x' was needed. The bare '&v' for
// address-of-the-ref-itself must still suppress, so the gate now
// requires the AMP's operand to be the bare ref-param ident.
struct S { int x; };

int read_via_ref(S*& v) { return (v ? &v->x : 0)[0]; }

int main() {
    S s; s.x = 7;
    S *sp = &s;
    return read_via_ref(sp);
}
