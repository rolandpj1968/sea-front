// EXPECT: 0
// Implicit memberwise op= for class B whose only members are arrays
// of class-type A — N4659 §15.8.2/12 [class.copy.assign]. Per-
// element A::op= must fire (its side effects are observable; the
// bitwise C-level struct copy would skip them).
//
// Two shapes:
//   - Single-dim array (mirrors g++.dg/other/copy2.C).
//   - Multi-dim array (mirrors g++.dg/init/array18.C).
//
// Sea-front detects "class has array-of-class-with-user-op="
// at the ND_ASSIGN site and emits a stmt-expr that calls A::op=
// per element via nested loops (covers any number of dimensions).

int single_dim_calls = 0;
int multi_dim_calls = 0;

struct A1 {
    A1() {}
    A1 &operator=(const A1 &) { ++single_dim_calls; return *this; }
};

struct A2 {
    A2() {}
    A2 &operator=(const A2 &) { ++multi_dim_calls; return *this; }
};

struct B1 { A1 arr[5]; };
struct B2 { A2 grid[2][3]; };

int main() {
    {
        B1 a, b;
        a = b;
        if (single_dim_calls != 5) return 1;
    }
    {
        B2 a, b;
        a = b;
        if (multi_dim_calls != 6) return 2;
    }
    return 0;
}
