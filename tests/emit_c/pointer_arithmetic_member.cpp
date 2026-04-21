// EXPECT: 7
// Sema must propagate a pointer type through 'ptr + int' so
// subsequent member accesses resolve. Previously common_arith_type
// returned NULL for pointer operands (neither is 'arithmetic'),
// leaving 'p + 0' with NULL resolved_type — then '(p + 0)->field'
// couldn't resolve the member, cascading to downstream failures
// (e.g. method dispatch on '.iterate()'). N4659 §8.7 [expr.add],
// C11 §6.5.6.
// Pattern from gcc 4.8 function.h: '#define cfun (cfun + 0)'.
struct Box {
    int v;
    int get() const { return v; }
};

int main() {
    Box b;
    b.v = 7;
    Box* p = &b;
    // (p + 0) must retain pointer type so ->get() dispatches as a
    // method call, not as field access on an untyped result.
    return (p + 0)->get();
}
