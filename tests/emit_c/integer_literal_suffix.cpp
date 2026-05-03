// EXPECT: 1
// Integer-literal suffix must be preserved through codegen. Dropping
// the L from `1L << 32` produces `1 << 32`, which is undefined behavior
// on platforms where int is 32 bits — and on gcc 4.8 it caused
// gencondmd to mis-evaluate __builtin_constant_p(...) at runtime,
// emitting `0` (CODE_FOR_nothing) for many insn condition entries.
// That collapsed the i386 insn-codes enum and produced duplicate-case
// errors when compiling i386.c.
//
// N4659 §5.13.2 [lex.icon]: integer-literal type depends on suffix.
extern "C" int printf(const char*, ...);

unsigned long g_flags = (1UL << 38);
int main() {
    // If `1UL << 38` is emitted as `1 << 38`, the result is UB on a
    // 32-bit int target and the bit isn't set; the AND below would
    // yield 0. With the L suffix preserved, the bit is at position 38.
    return ((g_flags & (1UL << 38)) != 0) ? 1 : 0;
}
