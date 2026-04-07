// EXPECT: 15
// Method with a parameter, called via pointer-to-instance ('->').
// The call-site rewriter recognises 'ap->add(5)' (member access on a
// TY_PTR(struct)) and emits 'Adder_add(ap, 5)' — no '&' because ap is
// already a pointer.
struct Adder {
    int base;
    int add(int x) { return base + x; }
};

int main() {
    Adder a;
    a.base = 10;
    Adder *ap = &a;
    return ap->add(5);
}
