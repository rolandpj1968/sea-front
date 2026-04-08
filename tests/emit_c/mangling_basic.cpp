// EXPECT: 42
// Mangling framework smoke test — see docs/mangling.md and
// src/codegen/mangle.c. The human-readable scheme produces:
//
//   class tag        →  sf__<Class>
//   ctor             →  sf__<Class>__ctor
//   dtor wrapper     →  sf__<Class>__dtor
//   user dtor body   →  sf__<Class>__dtor_body
//   method           →  sf__<Class>__<method>
//
// All sea-front-emitted symbols start with 'sf__' so they live in
// their own namespace and never collide with C library functions.
// The double-underscore separator distinguishes scope steps from
// the underscores users may have in their own names.
//
// This test exercises ctor + dtor + method + namespace prefix all
// in one program. The 'Wrap' member with its own dtor exercises the
// nested dtor chain (sf__Outer__dtor calls sf__Wrap__dtor on the
// member after running sf__Outer__dtor_body).

struct Wrap {
    int w;
    Wrap() : w(40) {}
    /* Non-empty dtor body forces emission of sf__Wrap__dtor_body
     * (the user's body) and sf__Wrap__dtor (the wrapper that
     * chains member dtors after running the body). An empty dtor
     * body classifies as trivial and gets elided entirely. */
    ~Wrap() { w = 0; }
};

struct Outer {
    Wrap inner;
    int extra;
    Outer() : inner(), extra(2) {}
    ~Outer() {}
    int total() { return inner.w + extra; }
};

int main() {
    Outer o;
    return o.total();
}
