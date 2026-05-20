// EXPECT: 0
// EH: when 'throw A()' is the throw expression and A() itself throws,
// the ctor's exception is what propagates — the outer throw is never
// reached because the inner throw already unwinds.
//
// Regression for g++.dg/eh/elide1.C — the prior emit dropped the
// A() ctor call entirely and lowered to '__SF_THROW_CLASS(&(A){0},
// 0, lbl)', a zero-init compound literal. That skipped the throw
// inside A() and made the outer try catch a bogus class-typed throw
// instead of the int the test expected.
//
// Fix: when the throw operand is a zero-arg ctor call on a class
// with a user-declared default ctor (has_default_ctor), call the
// ctor explicitly in a stmt-expr, then __SF_CHAIN_THROW to skip the
// outer throw if the ctor itself unwound.

struct A {
    A() { throw 0; }
};

int main() {
    try {
        throw A();
    } catch (int i) {
        return i;
    } catch (...) {
        return 2;
    }
    return 3;
}
