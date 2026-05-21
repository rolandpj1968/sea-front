// EXPECT: 0
// Real-to-complex conversion ranks one tier worse than real-to-real
// in overload resolution — N4659 §16.3.3.2 [over.ics.rank] plus
// gcc's PR c++/31780 ranking. For `f(int)` invocation with
// candidates f(double) and f(_Complex int), f(double) wins (int →
// double is an integer/floating Conversion, int → _Complex int is
// a real-to-complex Conversion, ranked worse).
//
// Mirrors g++.dg/ext/complex3.C; ics_rank in sema.c grew complex-
// aware ranking with the int-vs-_Complex-int check pulled in
// before types_equivalent (which treats `_Complex int` and `int`
// as structurally identical and would otherwise hide the rank
// difference).

int complex_overload_picks = 0;
int double_overload_picks = 0;

void f(_Complex int) { ++complex_overload_picks; }
void f(double) { ++double_overload_picks; }

int main() {
    f(1);          // int arg — should pick f(double)
    f(2);          // same

    if (complex_overload_picks != 0) return 1;
    if (double_overload_picks  != 2) return 2;
    return 0;
}
