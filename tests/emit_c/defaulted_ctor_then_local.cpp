// EXPECT: 0
// `A::A() = default;` at namespace scope must NOT shadow the class
// name `A` for downstream lookups. Sea-front used to register the
// defaulted-ctor declarator name as ENTITY_VARIABLE before the
// promote-to-func step ran, so subsequent `A x;` at block scope
// looked up `A` and found the ctor variable (not the class type),
// failing with "expected ';' got IDENT".
//
// N4659 §15.1/1 [class.ctor]: "Constructors do not have names."
// The regular func-def branch already skipped the variable-register
// for ctor/dtor (see the parallel guard around the func.name
// region_declare); parse_declaration now mirrors that guard at the
// pre-promotion site.
//
// Reduced from g++.dg/cpp0x/defaulted1.C.

extern "C" void abort();

struct A {
    int v;
    A();
};

A::A() = default;

void f() {
    A x;
    x.v = 7;
    if (x.v != 7) abort();
}

int main() {
    f();
    return 0;
}
