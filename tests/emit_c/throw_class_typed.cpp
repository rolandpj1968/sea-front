// EXPECT: 0
// Class-typed throw + catch dispatches on per-class typeinfo.
// N4659 §15.3 [except.handle] + §18.1 [except.throw]. sea-front's
// EH phase 2 originally only matched primitive (`__sf_typeinfo_int`)
// throws; class throws stashed exc_type=0 and caught via uintptr_t
// cast, which truncates struct values. With the per-class
// typeinfo registry:
//   - throw of class T sets exc_type = &__sf_typeinfo_<T>.
//   - catch (T) compares against the same typeinfo pointer.
//   - catch (T) by value copies the heap-resident exception object.
//   - catch (T&) takes a pointer alias.

extern "C" void abort(void);

struct E {
    int v;
};

struct F {
    int x;
};

int main() {
    /* Throw an E; catch (E) — by value. */
    try {
        throw E{42};
    } catch (E e) {
        if (e.v != 42) abort();
    }

    /* Multiple handlers — pick the matching one by typeinfo. */
    try {
        throw F{7};
    } catch (E) {
        abort();   /* WRONG handler */
    } catch (F f) {
        if (f.x != 7) abort();
    }

    /* catch (E&) — pointer alias to the exception object. */
    try {
        throw E{99};
    } catch (E& e) {
        if (e.v != 99) abort();
    }

    return 0;
}
