// EXPECT: 0
// `obj.~Class()` / `p->~Class()` is an explicit destructor call,
// N4659 §8.2.5/4 [expr.ref] + §15.4 [class.dtor]. Sea-front used
// to mangle the trailing `Class` token without distinguishing it
// from a constructor, so the call linked against the ctor symbol
// (`_ZN1C1CEv` instead of `_ZN1CD1Ev`) and either link-failed
// or invoked the wrong function.
//
// Coverage:
//   - User-declared non-trivial dtor → routes to the mangled
//     dtor wrapper.
//   - User-declared but EMPTY dtor (still trivially destructible
//     in sea-front's hash) → lowers to `((void)(operand))` per
//     N4659 §15.4/15: "Calls to the destructor of a trivially
//     destructible class are permitted but unnecessary."
//
// Reduced from g++.dg/opt/pr96722.C.

extern "C" void abort(void);

int g_count = 0;

struct Nontrivial {
    int s;
    ~Nontrivial() { ++g_count; }
};

struct Trivial {
    int s;
    ~Trivial() {}
};

int main() {
    Nontrivial *p1 = new Nontrivial;
    p1->~Nontrivial();
    if (g_count != 1) abort();

    Trivial t{};
    t.~Trivial();  // no-op but must not link-fail

    return 0;
}
