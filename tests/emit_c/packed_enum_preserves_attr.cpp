// EXPECT: 0
// `enum E { ... } __attribute__((packed))` — GNU extension. Sea-front
// preserves the attribute on the C enum definition so the back-end cc
// honours the minimal-underlying-type semantics.
//
// Caveat: ISO C makes enumerator constants type 'int' regardless of
// the enum's underlying type, so `sizeof(xyzzy)` still yields 4 in
// the lowered C. That's a C-vs-C++ semantic gap, not a sea-front
// transpile gap — this test pins down what we DO preserve:
//   - the attribute round-trips
//   - the enum type variable's storage is the packed width
//
// Pattern: g++.dg/ext/packed7.C uses sizeof(enumerator) and so still
// fails on the C-language enumerator-type rule; we expose the
// attribute correctly here via sizeof(enum_var).

enum E { only_value = 3 } __attribute__((packed));

int main() {
    enum E v = only_value;
    // sizeof(enum E) (the enum type) must reflect the packed width —
    // gcc-as-C honours the attribute.
    return sizeof(v) == 1 ? 0 : 1;
}
