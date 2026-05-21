// EXPECT: 0
// `: arr()` for an array of class with a virtual function value-
// initializes each element: zero-init the storage THEN default-
// construct (which writes the vtable pointer + runs the body, if
// any). N4659 §11.6/8 [dcl.init] — value-init = zero-init + then
// default-construct. Without the pre-zero, a polymorphic class
// has the vtable pointer written by the synth ctor but its non-
// static data members keep whatever the underlying storage held.

struct Elt {
    virtual void f();
    char c;
};

void Elt::f() {}

struct Holder {
    Elt buf[16];
    Holder() : buf() {}
};

void *operator new(unsigned long, void *p) { return p; }

int main() {
    // Pre-fill storage with non-zero bytes. After ctor, each
    // Elt's `c` field must be 0 (zero-init), but the vtable
    // pointer must be valid (default-construct overwrites it).
    unsigned char storage[sizeof(Holder)];
    for (unsigned i = 0; i < sizeof(storage); i++) storage[i] = 0xCC;

    Holder *h = new (storage) Holder();
    for (int i = 0; i < 16; i++)
        if (h->buf[i].c != 0) return 1;

    // Verify the vtable was written by calling the virtual.
    // (If vtable were zero, the call would segfault.)
    h->buf[0].f();
    return 0;
}
