// EXPECT: 0
// A struct-returning method call sits inside the right operand of a
// short-circuit '||'. The hoist-rvalue-to-temp transform must NOT
// lift the call out of the operand; otherwise the call runs even when
// the LHS short-circuits true and the gate intended to skip it.
//
// Real-world bite: gcc 4.8 cgraph.c
//   gcc_checking_assert(!virtual_offset
//                       || tree_to_double_int(virtual_offset)
//                          == double_int::from_shwi(virtual_value));
// The lifted tree_to_double_int(virtual_offset) ran on a NULL pointer
// and crashed cc1plus during the build of every covariant / multi-
// inheritance / devirt code path.

struct DI {
    int v;
    bool eq(const DI &o) const { return v == o.v; }
};

DI make_di(int *p) {
    DI d;
    d.v = *p;            // crashes if p==NULL — proves the gate is honoured
    return d;
}

bool check(int *p, DI other) {
    return !p || make_di(p).eq(other);
}

int main() {
    DI d;
    d.v = 42;
    // Pass NULL: the '!p' is true, '||' short-circuits, make_di must
    // NOT run. If sea-front hoisted make_di(p) ahead of the gate, we'd
    // dereference p=NULL and segfault.
    if (!check((int *)0, d)) return 99;
    int x = 7;
    DI seven;
    seven.v = 7;
    if (!check(&x, seven)) return 98;
    return 0;
}
