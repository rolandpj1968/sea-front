// EXPECT: 0
// Class with an array-of-class member where the element's ctor can
// throw: when an element ctor throws mid-array, the already-
// constructed elements (0..k-1) must be destroyed in reverse, then
// the throw propagates to the enclosing try/catch. N4659 §15.2/2
// [except.ctor].
//
// Mirrors g++.dg/init/array16.C with smaller N. Uses `new holder()`
// (rather than a local `holder h;`) so the new-expression's chain-
// throw check propagates the ctor's exception to the catch — the
// var-decl path doesn't yet wire this up for classes without
// has_dtor (see g++.dg/init/array5.C / commit notes).

extern "C" void *malloc(unsigned long);
extern "C" void free(void *);

int ctor_calls = 0;
int dtor_calls = 0;
int throw_at  = -1;

struct elt {
    int idx;
    elt() : idx(ctor_calls) {
        ++ctor_calls;
        if (ctor_calls == throw_at) throw 2;
    }
    ~elt() { ++dtor_calls; }
};

struct holder {
    elt buf[8];
    holder() : buf() {}
};

int main() {
    /* Case 1: throw on first element — chain propagates, no
     * partial destruction. */
    ctor_calls = dtor_calls = 0;
    throw_at = 1;
    try {
        holder *p = new holder();
        free(p);
        return 1;                       // not reached
    } catch (...) {
        if (ctor_calls != 1) return 2;
        if (dtor_calls != 0) return 3;
    }

    /* Case 2: throw mid-array — partial destruction fires for
     * elements [0..k-1] inside the array-mem-init loop. */
    ctor_calls = dtor_calls = 0;
    throw_at = 4;
    try {
        holder *p = new holder();
        free(p);
        return 4;                       // not reached
    } catch (...) {
        if (ctor_calls != 4) return 5;
        if (dtor_calls != 3) return 6;
    }
    return 0;
}
