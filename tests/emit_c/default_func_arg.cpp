// EXPECT: 42
// Function parameter default arguments — N4659 §11.3.6 [dcl.fct.default]:
//   void f(int x, int y = 5);
//   f(10);   // equivalent to f(10, 5)
// sea-front captures default-value expressions at parse time (on the
// ND_PARAM node), collects them into Type->param_defaults alongside
// params, and injects them at call sites that pass fewer args than the
// function has params. All tail params (nargs..nparams-1) must have
// defaults — otherwise the arity mismatch stands and emit falls back
// to the user args as-is (producing a clear 'too few arguments' C
// diagnostic rather than silent miscompile).
//
// Pattern: gcc 4.8 expr.h
//   extern void emit_cmp_and_jump_insns (rtx, rtx, rtx_code, rtx,
//                                        machine_mode, int, rtx,
//                                        int prob=-1);
// Called with 7 args in asan.c and builtins.c.

static int add3(int a, int b = 10, int c = 20) { return a + b + c; }

static int add4(int a, int b, int c = 5, int d = 7) { return a + b + c + d; }

int main() {
    int x = add3(1);          // 1 + 10 + 20 = 31
    int y = add3(1, 2);       // 1 + 2 + 20 = 23
    int z = add4(1, 2);       // 1 + 2 + 5 + 7 = 15
    int w = add4(1, 2, 3, 4); // 1 + 2 + 3 + 4 = 10
    return 42;
}
