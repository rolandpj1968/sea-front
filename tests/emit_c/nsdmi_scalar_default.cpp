// EXPECT: 42
// Non-static data member initializer (NSDMI) for scalar members —
// 'struct B { int a = 42; };'. The default-member-init must:
//   1. NOT be emitted as 'int a = 42;' inside the struct body (C
//      forbids inline initializers on struct members);
//   2. Trigger synthesis of a default ctor for B if there isn't
//      one user-declared;
//   3. Be applied as 'this->a = 42;' in the synthesized ctor body.
// N4659 §12.6.2/9 [class.base.init].

struct B {
    int a = 42;
};

int main() {
    B b;
    return b.a;
}
