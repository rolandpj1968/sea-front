// EXPECT: 10
// Range-based for over a braced-init-list:
//
//   for (auto k : { e0, e1, e2, ... }) ...
//
// N4659 §9.5.4 [stmt.ranged]/1 + §11.6.4 [dcl.init.list]: the range
// is treated as a std::initializer_list<E> temp; iteration walks its
// backing array. Sea-front emits the backing array as a block-local
// `const E[N]` and indexes it directly — equivalent to iterating
// begin()/end() on a synthesised initializer_list, but without the
// detour through the std::initializer_list type itself.
//
// Pattern: g++.dg/cpp0x/initlist105.C, initlist106.C.

int main() {
    int sum = 0;
    int a = 1, b = 2, c = 3, d = 4;
    // Block-local: elements are non-constant runtime values, which
    // verifies the const-storage-not-static choice in the lowering.
    for (auto k : {a, b, c, d}) sum += k;
    return sum;
}
