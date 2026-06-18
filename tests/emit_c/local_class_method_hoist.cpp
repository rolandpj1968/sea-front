// EXPECT: 0
// Local class declared inside a method body, with its own
// method bodies. N4659 §9.8 [class.local] permits this; C
// forbids function definitions at block scope, so the local
// class's struct + method bodies must be emitted at file scope.
//
// Sea-front's TU emit driver builds an EmitUnit list with a
// pre-pass; local class defs encountered while walking
// function bodies are registered as units BEFORE the enclosing
// function unit. Emission then visits them at file scope first;
// the in-body inline emit detects codegen_emitted and skips.
//
// Pattern reduced from g++.dg/eh/spec7.C — the dg version uses
// a virtual-dispatch + throw flow that combines this fix with
// EH machinery. The standalone shape below isolates the hoist.

extern "C" void abort();

int g_dtor_count = 0;
int g_outer_calls = 0;

struct Outer {
    int v;
    Outer() : v(0) {}

    void run() {
        ++g_outer_calls;

        /* Local class with a dtor body — the dtor symbol
         * `_ZZN5Outer3runEvEN1OD2Ev`-ish needs file-scope
         * definition. */
        struct Tracker {
            int id;
            Tracker(int i) : id(i) {}
            ~Tracker() { ++g_dtor_count; }
        };

        Tracker a(1);
        Tracker b(2);
        v = a.id + b.id;
    }
};

int main() {
    {
        Outer o;
        o.run();
        if (o.v != 3) abort();
        if (g_outer_calls != 1) abort();
        if (g_dtor_count != 2) abort();  // both Trackers destroyed
    }
    return 0;
}
