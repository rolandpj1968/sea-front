// EXPECT: 0
// C++ rule: an enumerator constant has the enumeration type — N4659
// §10.2/8 [dcl.enum]. So `sizeof(enumerator)` matches the enum's
// underlying-type size, which __attribute__((packed)) narrows to
// the minimum that fits the enumerators.
//
// The C-language equivalent ascribes type `int` (size 4) to every
// enumerator constant; sea-front lowers to C, so a packed enum's
// enumerator naively reads as sizeof(int).
//
// Fix: in sizeof emit, when the operand is an enumerator of a
// packed enum, cast to the enum type so sizeof sees the packed
// underlying. Pattern: g++.dg/ext/packed7.C.

enum XXX { xyzzy = 3 } __attribute__((packed));

int main() {
    return (sizeof(xyzzy) == 1) ? 0 : 1;
}
