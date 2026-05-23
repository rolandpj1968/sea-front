// EXPECT: 0
// N4659 §15.6.2/13 [class.base.init] — VIRTUAL bases construct
// FIRST, before non-virtual direct bases, before non-static
// members. Destruction is the reverse: members, then non-virtual
// bases, then virtual bases LAST.
//
// Sea-front used to ignore the virtual/non-virtual distinction
// and construct/destruct in pure declaration order. For a class
// declared as `A : public C1, C2, virtual public D, virtual
// public E`, that put C1/C2 BEFORE D/E in the ctor — the wrong
// order for the standard, and the reverse-declaration dtor then
// destroyed D/E in the middle of the unwind.
//
// Pattern: g++.dg/init/dtor1.C.

extern "C" void abort();

int d = 5;

struct B {
    int x;
    B(int i) : x(i) {}
    ~B() { if (d-- != x) abort(); }
};

struct C1 : public B { C1(int i) : B(i) {} };
struct C2 : public B { C2(int i) : B(i) {} };
struct D  : public B { D(int i)  : B(i) {} };
struct E  : public B { E(int i)  : B(i) {} };

struct A : public C1, C2, virtual public D, virtual public E {
    A() : D(0), E(1), C1(2), C2(3), x1(4), x2(5) {}
    B x1;
    B x2;
};

int main() {
    A a;
    // Ctor must have run D(0), E(1), C1(2), C2(3), x1(4), x2(5).
    // d still 5 here.
    // ~A destructs reverse: x2, x1, C2, C1, E, D
    //   ~x2 sees d=5 x=5; d→4
    //   ~x1 sees d=4 x=4; d→3
    //   ~C2 sees d=3 x=3; d→2
    //   ~C1 sees d=2 x=2; d→1
    //   ~E  sees d=1 x=1; d→0
    //   ~D  sees d=0 x=0; d→-1
    return 0;
}
