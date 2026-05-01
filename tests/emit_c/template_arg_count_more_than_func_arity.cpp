// EXPECT: 1
// Regression: free-function template with more template parameters than
// function parameters needed a SubstMap with capacity sized to template-
// param count, not function-param count. Otherwise deduction silently
// dropped bindings beyond function arity, build_template_id_from_deduced
// returned NULL, and the call emitted unmangled (link-time miss).
//
// Pattern: gcc 4.8 vec.h
//   template<typename T, typename A>
//   void gt_pch_nx(vec<T, A, vl_embed> *v);
// — 2 template params (T, A), 1 function param. SubstMap capacity was
// sized to 1 (function arity); the second binding (A) was silently
// dropped, the call to the 1-arg overload stayed unmangled, and
// every gt_pch_nx<T, va_gc> instantiation went undefined at link time.
//
// Standard: N4659 §17.8.2.1 [temp.deduct.call]/1 — template arguments
// are deduced from each call argument by comparing parameter and
// argument types; the SubstMap must have a slot per template parameter.

template<typename T, typename A> struct vec {};

template<typename T, typename A>
void foo(vec<T, A> *p) { (void)p; }

template<typename T, typename A>
void foo(vec<T*, A> *p, int a, int b) { (void)p; (void)a; (void)b; }

template<typename T, typename A>
void foo(vec<T, A> *p, int x) { (void)p; (void)x; }

struct Bar {};
struct Alloc {};

int main() {
    vec<Bar, Alloc> v;
    foo(&v);
    return 1;
}
