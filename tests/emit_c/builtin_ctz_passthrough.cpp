// EXPECT: 12
// __builtin_ctzl(x) must pass through as a real call, not be swallowed
// by the type-trait heuristic. Regression: when __builtin_ctzl returned
// 0 instead of the trailing-zero count, gcc 4.8's ggc allocator wedged
// at runtime (lg_pagesize = exact_log2(4096) = ctz_hwi(4096) → 0 instead
// of 12, packing pages 1 byte apart and corrupting tree-node layout).
//
// The bug: src/parse/expr.c treated *every* unknown __* identifier as a
// type-trait (returning opaque BOOL_LIT and discarding args). __builtin_*
// must flow through to the C compiler.
static inline int my_ctz(unsigned long x) {
    if (x == 0) return 64;
    return __builtin_ctzl(x);
}

int main() {
    return my_ctz(4096); // 12
}
