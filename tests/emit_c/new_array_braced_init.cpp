// EXPECT: 0
// `new T[N]{a0, a1, ...}` — array-new with a braced-init-list.
// Each brace item directly initialises one element with the
// matching single-arg ctor (C++11 §8.5.4 + §5.3.4 [expr.new]).
//
// Pre-fix sea-front skipped-and-discarded the brace contents
// in parse, then the codegen array path only handled the
// na==0 (default-init) case — `new T[2]{a, b}` ended up as
// `(T*)alloc(2*sizeof(T))` with NO ctor calls.
//
// Pattern: g++.dg/cpp0x/initlist49.C.

extern "C" void abort();

struct A {
    enum E { c_string, number } e;
    A(const char *) : e(c_string) {}
    A(int)         : e(number)  {}
};

int main() {
    A *p = new A[2]{1, ""};
    if (p[0].e != A::number)   return 1;
    if (p[1].e != A::c_string) return 2;

    // Three-element form with mixed ctors.
    A *q = new A[3]{"foo", 7, "bar"};
    if (q[0].e != A::c_string) return 3;
    if (q[1].e != A::number)   return 4;
    if (q[2].e != A::c_string) return 5;

    return 0;
}
