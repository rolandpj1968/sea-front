// EXPECT: 0
// Reference binding to a packed-struct member must materialise a
// properly-aligned temporary — the source location may be misaligned,
// so the language requires the compiler to insert the temp and bind
// the reference to it (N4659 §11.6.3 [dcl.init.ref] with platform
// alignment for `T&`).
//
// Test pattern: pass `p.i` (packed-member int) to a const-int& param
// AND `&p.i` separately as a plain int*. The function compares
// `&param == ptr_arg`. If sea-front emits `&p.i` directly for both,
// the comparison is true (regression). If sea-front materialises a
// temp for the reference, the addresses differ.
//
// Mirrors g++.dg/ext/packed4.C.

struct Unpacked { int i; };

int ConstRef(int const &p, int const *ptr) {
    return &p == ptr;   // expected 0 (temp differs from packed addr)
}

struct __attribute__((packed)) Packed {
    char c;
    int i;
};

int main() {
    Packed p;
    p.c = 0x12;
    p.i = 0x3456789a;
    return ConstRef(p.i, &p.i);
}
