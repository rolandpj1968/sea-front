// EXPECT: 0
// Itanium-ABI array cookie. `new T[N]` where T has a non-trivial
// destructor allocates an extra sizeof(size_t) header in front of
// the array storage and writes N there. delete[] reads N back to
// run per-element destructors over [0, N) and then frees the
// ORIGINAL allocation (i.e. cookie ptr, not the element ptr).
// N4659 §5.3.4/16 [expr.new] + §15.4/5 [class.dtor] + Itanium ABI
// §2.7. Pattern: g++.dg/cpp0x/defaulted19a.C.
//
// Triviality (per N4659 §15.4/5): a destructor is trivial iff it
// is not user-provided AND every member/base also has a trivial
// destructor. `=delete` does NOT count as user-provided.

extern "C" void *malloc(unsigned long);
extern "C" void free(void *);
extern "C" void abort(void);

int g_dtor_count;
int g_alloc_count;

void *captured_alloc;
void *operator new[](unsigned long sz) {
    ++g_alloc_count;
    captured_alloc = malloc(sz);
    return captured_alloc;
}

struct Tracked {
    ~Tracked() { ++g_dtor_count; }
};

struct WithSubobject {
    Tracked t;
    ~WithSubobject() = delete;  // doesn't defeat trivial-dtor when no body
};

struct TriviallyDestructible {
    int x;
};

int main() {
    /* Element with user-provided dtor → cookie allocated, dtors run. */
    Tracked *t = new Tracked[5];
    if ((void *)t == captured_alloc) abort();   // cookie offset
    delete[] t;
    if (g_dtor_count != 5) abort();             // all dtors ran

    /* Element class with =delete dtor but a member that has user-
     * provided dtor → still cookie (B's dtor runs). */
    g_dtor_count = 0;
    WithSubobject *w = new WithSubobject[3];
    (void)w;
    if ((void *)w == captured_alloc) abort();   // cookie offset

    /* Trivially destructible element → no cookie. */
    g_alloc_count = 0;
    TriviallyDestructible *p = new TriviallyDestructible[4];
    if ((void *)p != captured_alloc) abort();   // no cookie offset
    delete[] p;

    return 0;
}
