// EXPECT: 0
// N4659 §14.3 [class.friend]: a friend function defined inside a
// class body has namespace scope — its body must be emitted at
// file scope to match. Sea-front parsed the syntax but the body
// never reached codegen: the in-class deferred-body replay loop
// only unwrapped ND_TEMPLATE_DECL, missing the ND_FRIEND wrapper.
// And the friend func wasn't registered in the free-function
// overload set (free_ovld_walk skipped class members), so an
// overloaded friend collided with a top-level overload at link
// time.
//
// Two fixes:
//
//   1. Deferred-body replay also unwraps ND_FRIEND to reach the
//      inner ND_FUNC_DEF.
//
//   2. free_ovld_walk descends into ND_CLASS_DEF members and
//      recurses friend wrappers, so overloaded friends get
//      distinct mangled symbols at emit time.
//
// EmitOrder also registers each friend func def as a TU-level
// unit (separate from the local-class hoist machinery — friends
// don't need a back-edge to the enclosing class).

extern "C" void abort();

int probe_calls = 0;

struct Owner {
    int v;
    Owner(int x) : v(x) {}

    /* In-class friend with a body — most common shape. The
     * friend must be emittable + callable. Zero-arg form keeps
     * the test focused on the body-emit machinery; class-arg
     * friends (ADL-found) need separate name-lookup work. */
    friend int probe() { ++probe_calls; return 42; }
};

int main() {
    if (probe() != 42) abort();
    if (probe_calls != 1) abort();

    Owner o(7);   /* Owner ctor runs without referencing probe. */
    if (o.v != 7) abort();

    return 0;
}
