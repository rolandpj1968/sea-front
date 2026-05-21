// EXPECT: 0
// Placement new dispatches to the user's `operator new(size_t, void*)`
// and threads the placement arg through. The user-defined placement
// new returns the placement pointer; the test verifies the
// constructed object lives at that exact address. N4659 §8.3.4
// [expr.new].

extern "C" int printf(const char *, ...);

int gcount = 0;

struct A {
    int x;
    A(int v) : x(v) { ++gcount; }
};

// User-defined placement new — just returns the placement arg.
void *operator new(unsigned long, void *p) {
    return p;
}

int main() {
    unsigned char storage[sizeof(A)];
    A *a = new (storage) A(42);

    // The new-expression must have constructed A AT `storage`.
    if ((void *)a != (void *)storage) return 1;
    if (a->x != 42) return 2;
    if (gcount != 1) return 3;
    return 0;
}
