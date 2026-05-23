// EXPECT: 0
// `delete p` where p is a non-primary base subobject pointer of a
// most-derived D allocation must free the ORIGINAL allocation
// (the D* start), not the base subobject pointer. Itanium ABI
// handles this with the "deleting destructor" (D0) variant in
// the vtable.
//
// Sea-front's vtable __dtor slot now returns void* (the most-
// derived `this` after MI adjustment); the delete codegen
// frees that returned pointer. For primary inheritance the
// return is `this` unchanged. For MI secondary inheritance,
// the thunk subtracts the subobject offset and returns the
// D* base.
//
// Pre-fix sea-front called `__builtin_free(b2_ptr)` directly
// — freeing a mid-allocation pointer that no malloc returned.
//
// Pattern: g++.dg/init/delete2.C.

extern "C" void abort();
extern "C" void *malloc(unsigned long);
extern "C" void free(void *);

// Stand-in for the global new/delete that sea-front's _Znwm/_ZdlPv
// shims route through (they call malloc/free directly, which IS
// what we want for the test — every malloc'd pointer comes back
// to free unchanged, so we just check pointer identity by
// recording the latest allocation and asserting `free(p)` sees
// the same one.
static void *last_alloc;
static bool freed_correctly = true;

void *operator new(unsigned long s) {
    void *p = malloc(s);
    last_alloc = p;
    return p;
}
void operator delete(void *p) {
    if (p != last_alloc) freed_correctly = false;
    free(p);
}

struct B1 {
    virtual ~B1() {}
    int x;
};
struct B2 {
    virtual ~B2() {}
    int y;
};
struct D : B1, B2 {
    ~D() {}
    int z;
};

void delete_via_B1(B1 *p) { delete p; }
void delete_via_B2(B2 *p) { delete p; }
void delete_via_D (D  *p) { delete p; }

int main() {
    delete_via_D (new D);   // straight D delete
    if (!freed_correctly) return 1;
    delete_via_B1(new D);   // primary base — B1 ptr == D ptr in bits
    if (!freed_correctly) return 2;
    delete_via_B2(new D);   // secondary base — needs thunk to adjust + return D*
    if (!freed_correctly) return 3;
    return 0;
}
