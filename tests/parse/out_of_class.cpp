// Out-of-class method definitions: 'void Foo::bar() { ... }'
// The body should resolve Foo's members (and its base classes')
// via the qualified declarator-id walker, not the IDENT-IDENT
// heuristic safety net.

struct Base {
    typedef long base_type;
};

struct Outer : Base {
    typedef int size_type;
    void method();
    int  multi(int);
};

// 'size_type' is a member of Outer, used inside an out-of-class
// method body. Real lookup walks Outer's class_region (pushed via
// qualified_decl_scope) so the type resolves to 'int'.
void Outer::method() {
    size_type n = 42;
    base_type b = 1;  // inherited from Base via Outer's chain
    (void)n; (void)b;
}

// Same with a parameter using a member type — exercises the
// param-list parser too (which uses parse_type_specifiers).
int Outer::multi(int x) {
    size_type local = x;
    return local;
}

// Namespace-qualified member function — Foo::bar where Foo is in a
// namespace.
namespace ns {
    struct Inner {
        typedef double value_type;
        value_type compute();
    };
}

ns::Inner::value_type ns::Inner::compute() {
    value_type v = 3;
    return v;
}
