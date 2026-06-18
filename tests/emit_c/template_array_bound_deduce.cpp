// EXPECT: 0
// N4659 §17.8.2.5/8 [temp.deduct.type]: when the parameter type
// is `T[I]` and the argument is `U[N]`, the array bound I is
// deducible from N. Sea-front's deduce_from_pair recursed on the
// element type (binding T = U) but never bound I — the synthesised
// instantiation key still carried the NTTP placeholder, and the
// overload was dropped from the candidate set.
//
// With the bound-deduction wired, `Foo(int[4])` against
// `template<typename T, unsigned long I> int Foo(T const (&)[I])`
// now binds T=int + I=4, instantiates Foo<int, 4>, and links.
//
// Reduced from g++.dg/template/deduce1.C.

extern "C" void abort();

template <typename T> int FooA(T const *) { return 1; }
template <typename T> int FooA(T const &) { return 2; }
template <typename T, unsigned long I>
int FooA(T const (&ref)[I]) { return 100 + (int)I; }

int main() {
    int arr4[4] = {};
    if (FooA(arr4) != 104) abort();      // T=int, I=4 → 104

    int arr7[7] = {};
    if (FooA(arr7) != 107) abort();      // T=int, I=7 → 107

    /* A non-array arg still picks the T const& overload. */
    int x = 5;
    if (FooA(x) != 2) abort();

    /* Pointer arg picks T const* overload. */
    int *p = arr4;
    if (FooA(p) != 1) abort();

    return 0;
}
