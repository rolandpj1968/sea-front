// EXPECT: 42
// Multi-inheritance with a virtual destructor: `delete pA` where pA
// is an A* into a D-subobject (A being the FIRST polymorphic base
// of D) must reach D's dtor through vtable dispatch and run the
// member-and-base destruction chain in reverse declaration order.
//
// Restricted to the primary base because delete-through-secondary-
// base needs offset-recovery in the deleting dtor — sea-front does
// not yet split into D0/D1 (deleting-vs-complete) variants. The
// dispatch path is the same for primary and secondary; the open
// question is only the free address.

int log_buf[16];
int log_pos = 0;

struct A {
    virtual ~A() { log_buf[log_pos++] = 1; }
};
struct B {
    virtual ~B() { log_buf[log_pos++] = 2; }
};
struct D : A, B {        // A is the primary (offset-0) base
    ~D() { log_buf[log_pos++] = 3; }
};

int main() {
    A *p = new D;        // primary base — offset 0; free(p) hits the malloc'd block
    delete p;            // dispatch via A's vptr → D's primary vtable → D's dtor
    if (log_pos != 3) return 99;
    // Expected order: D::~D() → B::~B() → A::~A()  (reverse declaration)
    if (log_buf[0] != 3 || log_buf[1] != 2 || log_buf[2] != 1) return 100;
    return 42;
}
