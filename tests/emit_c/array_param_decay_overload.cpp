// EXPECT: 42
// Array parameter mangles the same as pointer parameter.
// N4659 §11.3.4/5 [dcl.array]: array-as-parameter decays to
// pointer. The mangler must encode them identically so two
// overloads that differ only in array vs pointer notation
// don't accidentally produce distinct symbols. Pattern from
// gcc 4.8 gengtype.c set_gc_used_type (mangling fix).

// Two overloads that DIFFER only in second-arg type (int vs char).
// Both use array notation for the first arg. After array→ptr
// mangling decay, the first param contributes 'int_ptr' for both,
// and the second param differentiates the overloads.
int pick(int p[3], int n) {
    int total = 0;
    for (int i = 0; i < n; i++) total += p[i];
    return total;
}
int pick(int p[3], char c) {
    return p[(int)c];
}

int main() {
    int d[3] = { 10, 20, 12 };
    return pick(d, 3);   // resolves to int-arg overload, returns 42
}
