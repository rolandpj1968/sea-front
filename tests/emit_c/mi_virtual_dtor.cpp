// EXPECT: 42
// Multi-inheritance: `delete pB` where pB is a B* into a D-subobject
// must reach D's dtor even though dispatch goes through B's vtable
// view. The secondary vtable for D-as-B holds a __dtor thunk that
// adjusts B* down to D* before forwarding.

int log_buf[16];
int log_pos = 0;

struct A {
    virtual ~A() { log_buf[log_pos++] = 1; }
};
struct B {
    virtual ~B() { log_buf[log_pos++] = 2; }
};
struct D : A, B {
    ~D() { log_buf[log_pos++] = 3; }
};

int main() {
    B *p = new D;       // p points at D's B-subobject
    delete p;           // dispatch via B's vptr → secondary D-vtable → D::dtor thunk
    if (log_pos != 3) return 99;
    // Expected order: D::~D() → B::~B() → A::~A()
    // log = [3, 2, 1]   sum = 6, ordered check encoded:
    if (log_buf[0] != 3 || log_buf[1] != 2 || log_buf[2] != 1) return 100;
    return 42;
}
