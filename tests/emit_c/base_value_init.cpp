// EXPECT: 0
// Value-initialization of a base subobject — N4659 §11.6/8.3
// [dcl.init]:
//
//   "to value-initialize an object of type T means:
//    [...] otherwise, the object is zero-initialized."
//
// Pattern: a POD base with no user-declared ctor, value-initialized
// from a derived class's mem-init list ('Base()').
//
// Sea-front previously skipped the mem-init entirely for this shape
// because pod has_default_ctor == false (no synthesized ctor exists
// for trivial types). The derived object's pod base then carried
// indeterminate stack contents, breaking the test's 'i.i == 0' check.
//
// Real-world cluster: g++.dg/init/value7.C and friends test exactly
// this — derived types with value-initialized POD bases.

struct pod {
    int i;
};

struct inherit : pod {
    inherit() : pod() {}
};

int main() {
    // Stamp the stack before declaring inherit, so a zero result is
    // load-bearing — it confirms the base was zero-initialized rather
    // than getting lucky with stack residue.
    int volatile garbage[64];
    for (int i = 0; i < 64; i++) garbage[i] = 0xCDCDCDCD;
    (void)garbage;

    inherit obj;
    return obj.i != 0 ? 1 : 0;
}
