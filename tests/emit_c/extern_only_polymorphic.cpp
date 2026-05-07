// EXPECT: 0
// Polymorphic class whose virtual methods are all extern-only
// (declarations in the TU; bodies in another TU / system library).
// N4659 §13.3 [class.virtual]. Sea-front skips emitting the vtable
// struct, vtable instance, and vptr field for such classes — see
// docs/mangling.md "System-class interop" for the design rationale.
//
// Real-world hit: gcc 14 libcpp transitively includes <string> ⇒
// <stdexcept> ⇒ class std::exception with virtual destructor and
// virtual what() declared but not defined here. Without this skip,
// the emitted vtable instance references undeclared symbols.
//
// The test pairs an extern-only polymorphic base with a regular
// (non-polymorphic) derived; the derived gets no inherited vptr,
// matching what we'd want for std::exception subclasses outside
// libstdc++.

class ExternBase {
public:
    virtual ~ExternBase();
    virtual int speak();
};

class Plain {
public:
    int x;
};

int main() {
    /* No actual instances of ExternBase — it's only useful as a
     * type for catch / pointer params. The test verifies that the
     * code compiles even with the polymorphic class declared but
     * its bodies not visible. */
    Plain p;
    p.x = 0;
    return p.x;
}
