// EXPECT: 0
// Class with an explicit (user-defined) constructor whose mem-init
// list does NOT mention an array-of-class member: the synthesised
// ctor-mem-init walk must default-construct each element. The
// matching dtor (synthesised when the class has any dtor-bearing
// member) must destroy them in reverse order. N4659 §15.6.2/9
// [class.base.init] + §15.4 [class.dtor]/9.
//
// Mirrors g++.dg/init/ctor1.C: a ctor body that throws after the
// array member is fully constructed exercises both the ctor's
// per-element loop and the throw-unwind dtor. Includes a plain
// dtor-bearing struct member alongside the array so the class's
// has_dtor is set (which the chain machinery needs to recognise
// the local as cleanup-worthy and route the ctor's throw through
// the try handler).
//
// Note: bare `Outer o;` for a class with ONLY array-of-class
// members (no user ctor, no plain dtor-bearing member) currently
// skips array-element construction at the declaration site — the
// partial-destruction unwind for a throwing element ctor isn't
// emitted yet (tracked alongside g++.dg/eh/new1.C / array16.C).

int a_dtor_calls = 0;
int dtor_seq[16];
int dtor_seq_n = 0;

struct A {
    int idx;
    static int next;
    A() : idx(next++) {}
    ~A() { dtor_seq[dtor_seq_n++] = idx; ++a_dtor_calls; }
};
int A::next = 0;

int sentinel_dtor_runs = 0;
struct Sentinel { ~Sentinel() { ++sentinel_dtor_runs; } };

struct ThrowOnB {
    Sentinel s;        // ensures the class has has_dtor → chain fires
    A arr[3];
    ThrowOnB() { throw 1; }
};

int main() {
    try {
        ThrowOnB t;
        return 1;             // not reached
    } catch (...) {}
    /* Three A's constructed before the body's throw, three dtors
     * fired during unwind, in reverse order. */
    if (a_dtor_calls != 3)              return 2;
    if (dtor_seq[0] != 2 || dtor_seq[1] != 1 || dtor_seq[2] != 0)
                                         return 3;
    return 0;
}
