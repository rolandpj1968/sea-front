// EXPECT: 10
// Range-based for over a plain array whose extent was DEDUCED from
// the initializer (`int a[] = {1,2,3,4}`). The parsed Type may carry
// array_len = 0 in that case (the deduction happens after the type
// is built), so the codegen must fall back to
// 'sizeof(a)/sizeof(*a)' for the end pointer instead of refusing
// with "unsupported range-for".  N4659 §9.5.4 [stmt.ranged]/1.

int main() {
    int a[] = {1, 2, 3, 4};
    int sum = 0;
    for (int x : a)
        sum += x;
    return sum;
}
