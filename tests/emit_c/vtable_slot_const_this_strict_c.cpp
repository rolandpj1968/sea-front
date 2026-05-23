// EXPECT: 42
// A const-qualified virtual method has C signature
//   int meth(const struct T *this, ...);
// The vtable slot for that method must also be typed
//   int (*meth)(const struct T *, ...);
// Otherwise storing the method's function pointer into the slot
// drops const, which gcc accepts with a warning but strict-C back-
// ends like cproc reject as "base types of pointer assignment must
// be compatible".
//
// Sea-front emit-side fix: when the vtable slot is for a const
// method, prefix the this-arg type with 'const' in the slot
// declaration. The vtable instance initializer then matches the
// method's actual signature.

struct Base {
    virtual int peek() const { return 1; }
};

struct Derived : Base {
    virtual int peek() const { return 42; }
};

int main() {
    Derived d;
    Base *p = &d;
    return p->peek();
}
