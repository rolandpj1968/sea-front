// EXPECT: 0
// `: m()` mem-init for a non-class scalar member value-initializes
// it (zero-init for arithmetic / pointer types). N4659 §15.6.2/8
// [class.base.init]. Without the emit, the member keeps whatever
// the storage already held — observable when the storage was
// placement-new'd into a buffer with pre-existing non-zero bytes
// (g++.dg/init/array16.C exercises this in dg form; this is the
// reduced unit version).

extern "C" void *malloc(unsigned long);

struct S {
    int  i;
    char c;
    int *p;
    S() : i(), c(), p() {}
};

int main() {
    // Place an S into a buffer that's been pre-filled with non-
    // zero bytes; verify the ctor zeros i/c/p regardless.
    unsigned char buf[sizeof(S)];
    for (unsigned i = 0; i < sizeof(buf); i++) buf[i] = 0xCC;

    S *s = new (buf) S();
    if (s->i != 0)             return 1;
    if (s->c != 0)             return 2;
    if (s->p != (int *)0)      return 3;
    return 0;
}

// Provide the placement-new operator inline (not from <new>, which
// the unit-test runner doesn't always have on the include path).
void *operator new(unsigned long, void *p) { return p; }
