// EXPECT: 42
// A derived class's mem-init invokes the base ctor with FEWER args
// than the base ctor declares; default arguments fill the tail.
// N4659 §11.3.6 [dcl.fct.default]. Without the default-arg
// injection, the call mangled with the full-arity name but the
// emit only wrote the user's args — link failed with 'too few
// arguments'.
//
// Surfaced by gcc 14's libcpp build: rich_location's 3-arg ctor
// invoked with 2 args from encoding_rich_location's mem-init.

struct Base {
    int sum;
    Base(int x, int y = 35) : sum(x + y) {}
};

struct Derived : Base {
    Derived() : Base(7) {}    // omits y; default of 35 fills in
};

int main() {
    Derived d;
    return d.sum;             // 7 + 35 = 42
}
