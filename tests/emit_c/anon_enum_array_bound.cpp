// EXPECT: 7
// Anonymous enum inside a class body used as an array bound for
// a sibling data member — N4659 §10.2 [dcl.enum]: enumerators of
// an unnamed enum at class scope are class members. Since C has
// no class-member enumerators, sea-front hoists the anonymous
// enum to TU scope BEFORE emitting the struct so the array bound
// resolves. Conditional on subsequent members actually using the
// enum (libstdc++ trait-style 'enum {__value=N}' inside many
// structs would collide if hoisted unconditionally; see
// anon_enum_in_struct.cpp for that contrast).
//
// Real-world hit: gcc 14 libcpp/internal.h's per-class
// 'enum {M_EXPORT, ..., M_HWM}; cpp_hashnode *n_modules[M_HWM][2];'
// pattern.

struct C {
    enum { A, B, X, Y, Z, C_HWM };   // C_HWM = 5
    int data[C_HWM];
};

int main() {
    C c;
    c.data[0] = 7;
    c.data[1] = 1;
    c.data[2] = 2;
    c.data[3] = 3;
    c.data[4] = 4;
    return c.data[0];
}
