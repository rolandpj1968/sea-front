// EXPECT: 0
// Covariant virtual return: 'AB::getThis() : AB*' overrides
// 'A::getThis() : A*'. Sea-front's vtable slot for A's getThis()
// is typed 'A *(*)(A*)' — the slot's C-level return is A*. But
// sema treats 'a->getThis()' as returning AB* (the covariant
// declared type), so a comparison 'a->getThis() != ab' (where ab
// is AB*) is well-typed in C++.
//
// In emitted C the call expression's C-level type is A* (slot
// return), and we compare against AB* (ab). C requires the two
// pointer operands of '!=' to be compatible; strict-C back-ends
// (cproc) reject the mismatch with 'pointer operands to != are
// to incompatible types'.
//
// Sea-front emit-side fix: when a pointer comparison's LHS or
// RHS is an ND_CALL, wrap both sides in '(void *)' so the
// comparison is well-defined under strict C. Pointer equality
// through void* matches the byte-address comparison the source
// intends.

struct A { virtual A *getThis() { return this; } };

int main() {
    A *a = new A();
    if (a->getThis() != a) return 1;
    delete a;
    return 0;
}
