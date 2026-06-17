// EXPECT: 0
// `catch (T& p)` for a primitive T — the throw machinery stores the
// value in __sf_exc_state.exc_obj via uintptr_t cast, with no
// native storage for the catch ref to bind to. Sea-front now
// materialises a local `__sf_caught_<id>` (lifetime = handler
// scope), copies the recovered value into it, and binds the catch
// ref to its address. The ref's auto-deref machinery then walks
// the indirection correctly.
//
// Reduced from g++.dg/eh/alias1.C — pointer-to-pointer thrown,
// caught by const ref.

extern "C" void abort(void);

int i = 42;
int *p0 = &i;
typedef int **ipp;

int main() {
    try {
        throw &p0;
    }
    catch (const ipp &p) {
        /* p binds the int** value; **p is i = 42. */
        if (**p != 42) abort();
    }

    /* Single-pointer ref catch — `catch (int *&)` for `throw &i`. */
    try {
        throw &i;
    }
    catch (int *&q) {
        if (*q != 42) abort();
    }

    /* Plain primitive ref catch — `catch (int&)` for `throw 7`. */
    try {
        throw 7;
    }
    catch (int &r) {
        if (r != 7) abort();
    }

    return 0;
}
