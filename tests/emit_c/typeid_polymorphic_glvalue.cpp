// EXPECT: 0
// `typeid(glvalue)` where glvalue's static type is a polymorphic
// class must yield the DYNAMIC type's typeinfo, per N4659 §8.2.7/3
// [expr.typeid]. Sea-front previously emitted the static sentinel
// for ALL operands, so `typeid(aref) == typeid(b)` returned false
// when aref was an `A&` bound to a `B`.
//
// Fix:
//   1. vtable struct + instance gain a leading `__sf_typeid` slot
//      pointing at the class's `__sf_typeid_<sym>` sentinel.
//   2. ND_TYPEID emit detects polymorphic glvalue operand and routes
//      through `((vowner *)&op)->__sf_vptr->__sf_typeid` to recover
//      the dynamic type.
//
// Real-world hit: g++.dg/opt/rtti1.C
//   B b; A &aref = b; return typeid(aref) != typeid(b);  // expects 0

#include <typeinfo>

extern "C" void abort();

struct A {
    virtual ~A() { }
};

struct B : A { };

int main() {
    B b;
    A &aref = b;
    if (typeid(aref) != typeid(b)) abort();
    /* Direct value also goes through dynamic lookup (b is an lvalue
     * of polymorphic type) — equals itself trivially. */
    if (!(typeid(b) == typeid(b))) abort();
    /* Through the dynamic type, NOT the static. */
    A &aref2 = b;
    if (typeid(aref) != typeid(aref2)) abort();
    return 0;
}
