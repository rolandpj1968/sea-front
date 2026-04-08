// EXPECT: 7
// Out-of-class destructor definition. The class body declares
// '~Foo();' (no body); the body lives at namespace scope as
// 'Foo::~Foo() { ... }'. Both forms must be recognized as a
// destructor and resolve to the same Foo_dtor wrapper symbol so
// they link together.
//
// Pre-fix the in-class declaration was an ND_VAR_DECL+TY_FUNC+
// is_destructor that the class member scan in type.c didn't
// recognize as a dtor source — has_dtor stayed false, no
// Foo_dtor wrapper was synthesized, no auto-cleanup fired at
// end of scope, and the out-of-class body Foo_dtor_body was
// orphaned (had no caller).
//
// Symmetric with the ctor out-of-class fix from commit 599b501.
int g = 0;

struct Foo {
    int v;
    Foo();
    ~Foo();
};

Foo::Foo() { v = 7; }
Foo::~Foo() { g = v; }

int main() {
    {
        Foo a;
    }
    return g;
}
