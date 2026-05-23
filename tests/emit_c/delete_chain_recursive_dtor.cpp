// EXPECT: 0
// A class whose dtor `delete`s a `T*` member should recursively
// destroy the chain. Pre-fix sea-front cached the field's resolved
// Type from before the class's has_dtor was stamped, so the `delete
// prev` inside the dtor body lowered to a bare __builtin_free —
// missing the recursive dtor call.
//
// Pattern: g++.dg/opt/pr42508.C.

extern "C" void abort();

int v[10], vidx;

struct A {
    A *prev;
    int  i;
    ~A() {
        v[vidx++] = i;
        delete prev;   // must call ~A() on prev BEFORE freeing
    }
};

int main() {
    A *a1 = new A();
    A *a2 = new A();
    a1->prev = 0;
    a1->i = 1;
    a2->prev = a1;
    a2->i = 2;
    delete a2;
    // Order should be: ~a2 first (v[0] = 2), then ~a1 (v[1] = 1).
    if (vidx != 2 || v[0] != 2 || v[1] != 1) abort();
    return 0;
}
