// EXPECT: 0
// `delete p` looks up `operator delete` in the pointee's class
// scope first (N4659 §8.3.5/10 [expr.delete]); `::delete p` forces
// the global one. Sea-front used to always emit `_ZdlPv` (global).
//
// Three intersecting fixes meet here:
//   - parser tracks `::delete` vs `delete` on ND_UNARY via the new
//     is_delete_global_scope flag.
//   - class-scope `operator delete` is emitted as IMPLICITLY STATIC
//     per N4659 §16.5.3/1: the `this` parameter is omitted, so the
//     signature matches what callers pass.
//   - delete emit dispatches to the class operator delete symbol
//     (mangled `_ZN<class>dlEPv`) when one exists and the form is
//     unqualified; falls back to `_ZdlPv` otherwise. Reduced from
//     g++.dg/eh/delete1.C.

extern "C" void *malloc(unsigned long);
extern "C" void free(void *);
extern "C" void abort(void);

static int who_freed = 0;
void operator delete(void *p) throw() { who_freed = 1; free(p); }

struct Baz {
    static int self_count;
    void operator delete(void *p) throw() { ++self_count; free(p); }
    virtual ~Baz() {}
};
int Baz::self_count = 0;

int main() {
    Baz *p1 = new Baz;
    delete p1;            /* Baz::operator delete — bumps self_count */
    if (Baz::self_count != 1) abort();
    if (who_freed != 0) abort();   /* global NOT called */

    Baz *p2 = new Baz;
    ::delete p2;          /* global operator delete forced by ::  */
    if (who_freed != 1) abort();
    if (Baz::self_count != 1) abort();   /* unchanged */

    return 0;
}
