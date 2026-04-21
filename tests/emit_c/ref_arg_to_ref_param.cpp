// EXPECT: 100
// Passing a reference-typed argument to a reference-typed parameter:
// the caller must pass the pointer as-is (no & and no *), even when
// the arg is an ND_IDENT that names a ref-param (which would normally
// emit-expr to '(*name)' via the ref-param deref). Previously emitted
// '&(*other)' or similar. Pattern from gcc 4.8 vec::operator!= calling
// operator==(other).
struct X {
    int v;
    bool equal_to(const X& other) const { return v == other.v; }
    bool differ_from(const X& other) const { return !equal_to(other); }
};

int forward_equal(const X& a, const X& b) { return a.equal_to(b); }

int main() {
    X a; a.v = 5;
    X b; b.v = 5;
    X c; c.v = 6;
    // equal_to(a,b) -> 1; differ_from(a,c) -> 1; forward_equal(a,b) -> 1
    int r = 0;
    if (a.equal_to(b)) r += 50;
    if (a.differ_from(c)) r += 30;
    if (forward_equal(a, b)) r += 20;
    return r;  // 100
}
