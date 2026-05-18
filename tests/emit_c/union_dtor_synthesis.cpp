// EXPECT: 42
// When a union member has a non-trivial dtor, sea-front synthesises
// a dtor wrapper for the union. The wrapper's C signature must use
// the 'union' keyword for the receiver type — not 'struct' — or the
// C compiler rejects it as 'defined as wrong kind of tag'.
//
// Same shape for the synthesised default ctor and the dtor wrapper
// forward decl. N4659 §12.3 [class.union]/2: unions can hold members
// with non-trivial special member functions in C++11. Test pattern
// modelled on the U-with-C-member fragment of g++.dg/cpp0x/defaulted1.

struct C {
    int i;
    C() : i(7) {}
    ~C() {}
};

union U {
    C c;
};

int main() {
    return 42;
}
