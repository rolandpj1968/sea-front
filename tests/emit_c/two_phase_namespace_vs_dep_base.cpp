// EXPECT: 0
// Two-phase name lookup (N4659 §17.5.6 [temp.res]): a non-dependent
// unqualified name inside a template body is bound at template
// definition time (phase 1). When the template's base class is a
// template parameter T, T's members are NOT visible during phase 1
// — only names visible at the namespace scope of the template's
// definition are. The phase-1 binding survives instantiation, even
// if T → B introduces a same-named member B::foo via inheritance.
//
// Pattern: g++.dg/lookup/template1.C. Without the phase-1 freeze,
// `foo()` inside C<B>::caller rebinds to B::foo (returns 1)
// because B is now visible through the no-longer-dependent base.

class B { public: int foo() { return 1; } };
int foo() { return 0; }                          // namespace ::foo

template <class T>
class C : public T {
public:
    int caller() { return foo(); }              // non-dependent → ::foo
};

int main() {
    C<B> c;
    // c.caller() must return 0 (from ::foo), NOT 1 (from B::foo).
    return c.caller();
}
