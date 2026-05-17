// EXPECT: 61
// `delete pBase` where the dynamic type is `Derived` must invoke
// `~Derived()` first, then chain to `~Base()`. N4659 §15.4
// [class.dtor]/12 + §8.3.5 [expr.delete]/3 — when the static type
// of the deletee has a virtual destructor, the most-derived dtor
// runs.
//
// Three pieces being exercised together:
//   1. Virtual dtor lands in the vtable's __dtor slot.
//   2. `new Derived` runs Derived's ctor on malloc'd storage (so
//      the vptr in the Base subobject points at Derived's vtable).
//   3. `delete p` dispatches __dtor through the vptr, then frees.

int log_buf[16];
int log_pos = 0;

struct Base {
    virtual ~Base() { log_buf[log_pos++] = 1; }
};

struct Derived : Base {
    ~Derived() { log_buf[log_pos++] = 2; }
};

int main() {
    Base *p = new Derived;
    delete p;
    if (log_pos != 2) return 99;
    return log_buf[0] * 20 + log_buf[1] + 20;   // 2*20 + 1 + 20 = 61
}
