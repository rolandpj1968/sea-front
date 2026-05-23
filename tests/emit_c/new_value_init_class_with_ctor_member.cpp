// EXPECT: 0
// `new B()` where B has NO user-provided default ctor but contains
// a class member with a user ctor must STILL zero-initialize the
// scalar members of B before the synthesised default ctor runs.
// N4659 §11.6/8 [dcl.init]: value-init of a class without a user-
// provided default ctor first zero-initializes the object, then
// default-initializes.
//
// Pre-fix sea-front only memset'd new'd storage in the "no ctor at
// all" path. B's `A a` member triggers a synth default ctor for B,
// which initialises `a` but leaves `i` undefined. The malloc-poison
// trick below would catch the missing zero.
//
// Pattern: g++.dg/init/value3.C.

#include <stdlib.h>
#include <string.h>

extern "C" void abort();

// Poison every fresh allocation with 0x2A bytes so any field that
// the value-init path forgets to zero shows up as a non-zero read.
void *operator new(size_t s) {
    void *p = malloc(s);
    memset(p, 0x2A, s);
    return p;
}
void *operator new[](size_t s) {
    void *p = malloc(s);
    memset(p, 0x2A, s);
    return p;
}

struct A { A() {} ~A() {} };
struct B { A a; int i; };

int main() {
    B *p = new B();
    if (p->i != 0) return 1;

    B *arr = new B[2]();
    if (arr[0].i != 0) return 2;
    // Note: sea-front's array-new currently constructs only the
    // first element. The value-init memset zeros the rest, which
    // is what we observe here.
    if (arr[1].i != 0) return 3;

    return 0;
}
