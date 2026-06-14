// EXPECT: 0
// Variadic template `template<class... Args> void f(Args... args)`
// instantiated with an empty pack — `f()`. The function body uses
// the pack in two pack-expansion sites:
//   - placement-new initializer: `new(p) T(args...)` becomes
//     `new(p) T()` (value-init), which must zero the storage
//   - direct call: `g(args...)` becomes `g()`
//
// N4659 §17.5.3 [temp.variadic] for the pack binding.
// N4659 §17.5.3.4 [temp.variadic.expand] for pack-expansion in
// initializer / call-args lists.
// N4659 §8.5/8 [dcl.init] for scalar value-init from `T()` → 0.

// Provide the placement-new operator inline (not from <new>, which
// the unit-test runner doesn't always have on the include path).
void *operator new(unsigned long, void *p) { return p; }

int k = 5;
int g_called = 0;

void g() { g_called = 1; }

template <class... Args>
void f(Args... args) {
    new (&k) int(args...);   // empty pack → int() → *p = 0
    g(args...);              // empty pack → g()
}

int main() {
    f();
    if (k != 0)            return 1;
    if (g_called != 1)     return 2;
    return 0;
}
