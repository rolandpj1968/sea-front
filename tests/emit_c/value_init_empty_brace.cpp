// EXPECT: 0
// Empty-brace value-init `T x{};` on a class with no user-declared
// constructor — N4659 §11.6.1/8 [dcl.init]: value-initialization
// zero-initialises the storage. Sea-front previously die'd on
// "no matching overload for ctor on class A (0 args)"; the fix is
// to recognise the value-init path: the bare var-decl emit already
// appends ` = {0}` for has_ctor_init && ctor_nargs==0, so the
// ctor-call emission just returns when there's no overload to call.
//
// Pattern: g++.dg/cpp0x/initlist-value.C `A a{};`.

struct B {};                  // empty base — keeps A from being an aggregate
struct A : B { int i; };

int main() {
    A a{};                    // value-init: a.i must be zero
    A b{};
    b.i = 7;                  // make sure we can write to it
    return a.i + (b.i - 7);   // 0 + 0 = 0
}
