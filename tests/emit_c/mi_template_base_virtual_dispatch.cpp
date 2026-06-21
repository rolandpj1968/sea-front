// EXPECT: 42
// Multiple inheritance where one base is a template instantiation
// with pure-virtual methods. The derived class:
//   - inherits a virtual dtor + virtual method from the template base
//   - overrides the virtual method WITHOUT the `virtual` keyword
//   - has no user-declared constructors of its own
//
// At parse time, parse/type.c can't propagate has_virtual_methods /
// has_default_ctor / has_dtor from the template-id base because the
// base hasn't been instantiated yet. After Phase-3 instantiation
// finishes, refresh_inherited_class_flags runs over every class to
// re-propagate and re-mark implicit-virtual overrides — without
// this, `Derived d;` skips its ctor (no vptr install) and the
// virtual call segfaults.
//
// The codegen var-decl gate also reads has_default_ctor through the
// canonical class_def Type because var_decl.ty may be a separate
// Type copy that wasn't reached by the refresh pass.
//
// Real-world hit: g++.dg/torture/pr44535.C
// `D : public C, public FOO::A<char>` calling `x.Enum()` which
// virtual-dispatches to D::OnProv via the A<char>::Enum body.

extern "C" void abort();

namespace NS {
template<typename T>
class Base {
public:
    void run() { dispatch(); }
    virtual void dispatch() = 0;
    virtual ~Base() { }
};
}

class Empty { };

class Derived : public Empty, public NS::Base<char> {
public:
    int got;
    Derived() : got(0) { }
    void dispatch() { got = 42; }
};

int main() {
    Derived d;
    d.run();
    if (d.got != 42) abort();
    return d.got;
}
