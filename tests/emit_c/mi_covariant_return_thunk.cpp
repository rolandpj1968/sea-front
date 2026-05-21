// EXPECT: 0
// MI + covariant return: when an override returns a more-derived
// pointer than the base's slot, the secondary-vtable thunk must
// do TWO adjustments — `this` on entry (subtract subobject offset
// to get the derived `this`) AND return value on exit (add the
// subobject offset of the slot's pointee type within the
// override's class, so a B*-typed slot receives the address of
// the B subobject rather than the AB address).
//
// N4659 §10.3/5 [class.virtual]. Without the return-adjust, a
// call through `B *bp` to `bp->getThis()` returns an unadjusted
// `AB*` reinterpreted as `B*`; the result reads the wrong
// subobject. Pattern: g++.dg/inherit/covariant1.C.

class A {                       public: virtual A *getThis() { return this; } };
class B { int unused; /*offset!*/ public: virtual B *getThis() { return this; } };
class AB : public A, public B { public: virtual AB *getThis() { return this; } };

int main() {
    AB *ab = new AB();
    A  *a  = ab;
    B  *b  = ab;

    // Direct (no virtual dispatch): trivially OK.
    if (ab->getThis() != ab) return 1;

    // Dispatch through A* — first base, offset 0. No thunk
    // needed; the slot points directly at AB::getThis. Return
    // is AB* and the caller's A* IS the same address.
    A *via_a = a->getThis();
    if ((void *)via_a != (void *)ab) return 2;

    // Dispatch through B* — non-first base. The slot is a thunk
    // that this-adjusts the receiver AND return-adjusts AB* → B*.
    B *via_b = b->getThis();
    if ((void *)via_b != (void *)b) return 3;
    return 0;
}
