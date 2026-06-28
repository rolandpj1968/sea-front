// EXPECT: 0
// `T(x)` functional cast where T has no own copy ctor but inherits a
// base with a user copy ctor. Pre-fix sea-front emitted a bitwise C
// struct copy and skipped the base's body entirely; the side effect
// inside the base copy ctor never ran. Post-fix the same-class
// branch routes through emit_inline_copy_chain when
// class_transitively_needs_copy_call(T) is true, so the base's
// user-written copy ctor body executes for every subobject.
// N4659 §15.8.1 [class.copy.ctor].
//
// Companion to functional_cast_copy_preserves_arg.cpp (which covers
// the trivial-class shape) and copy_ctor_template_wins_overload.cpp
// (which exercises the template-ctor winner path). This one isolates
// the non-template inline-chain routing.

extern "C" void abort();

int base_copies = 0;

struct Base {
    int v;
    Base() : v(0) {}
    Base(const Base& b) : v(b.v) { base_copies++; }
};

struct Derived : Base { };

int main() {
    Derived d;
    d.v = 42;

    /* Functional-cast — without 987a728, bitwise C struct copy ran
     * and base_copies stayed 0. With the fix, Base::Base(const Base&)
     * fires once via the inline copy chain. */
    Derived d2 = Derived(d);
    if (d2.v != 42) abort();
    if (base_copies != 1) abort();

    /* Expression position too — same path, distinct evaluation. */
    (void)Derived(d);
    if (base_copies != 2) abort();

    return 0;
}
