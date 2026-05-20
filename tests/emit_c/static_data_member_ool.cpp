// EXPECT: 0
// OOL definition of a static data member must mangle the symbol so
// it matches the in-class declaration AND doesn't collide with any
// C symbol of the same name. N4659 §11.4.9 [class.static.data].
//
// Without this fix, 'int Foo::abort;' was emitted as bare 'int abort;'
// — cc warns "built-in function 'abort' declared as non-function"
// and the symbol collides with libc's abort at link.
//
// Pattern: g++.dg/init/array16.C — class with 'static int abort;'.

struct Foo {
    static int abort;     // Yes, deliberately shadows libc::abort
    static int count;
};

int Foo::abort = 7;       // would have clashed with ::abort
int Foo::count = 42;

int main() {
    if (Foo::abort != 7) return 1;
    if (Foo::count != 42) return 2;
    return 0;
}
