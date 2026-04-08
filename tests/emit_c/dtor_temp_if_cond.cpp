// EXPECT: 1
// Slice D-cond: a class temporary in an if condition must be
// destroyed BEFORE the then/else branches run (N4659 §6.7.7
// [class.temporary]/4 — temps die at end of full-expression).
//
// Lowered: declare a synthetic int outside the if, evaluate the
// cond into it inside a mini-block, fire temp dtors via cleanup
// chain, then test the synthetic.
//
//   int __SF_cond_0;
//   {
//       struct T __SF_temp_1;
//       T_ctor(&__SF_temp_1, 7);
//       __SF_cond_0 = (__SF_temp_1.v == 7);
//   __SF_cleanup_1: ;
//       T_dtor(&__SF_temp_1);
//       __SF_CHAIN_ANY(__SF_epilogue);
//   }
//   if (__SF_cond_0) { ... }
//
// The dtor fires (g = 1) before 'int x = g' captures, so x = 1.
//
// Pre-fix the temp was hoisted at the OUTER block level via the
// regular hoist path, so its dtor fired at end of main —
// observably AFTER the read of g, giving x = 0 and exit 0.
int g = 0;

struct T {
    int v;
    T(int x) { v = x; }
    ~T() { g = g + 1; }
};

int main() {
    if (T(7).v == 7) {
        int x = g;
        return x;
    }
    return -1;
}
