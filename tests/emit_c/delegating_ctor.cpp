// EXPECT: 0
// Delegating constructor — N4659 §15.6.2/6 [class.base.init]:
//   "A mem-initializer-id that designates the constructor's class
//    shall be the only mem-initializer; the constructor is a
//    delegating constructor, and the constructor selected by the
//    mem-initializer is the target constructor."
// Sea-front formerly emitted the V (virtual base) init from the
// delegating ctor body but DROPPED the call to the target ctor —
// member sub-objects stayed uninitialised and the delegation
// flowed in name only.
//
// Limitation: class-typed mem-init args (`B(int i) : B(A(i)) {}`)
// need the hoisted-temp's dtor wired into the ctor's cleanup
// chain — which requires cf_begin_function to see the hoist before
// the body emits. Currently the temp's dtor doesn't fire, leaving
// a leak. dg test dc6 still FAILs for this reason.

extern "C" void abort();

int target_count = 0;

struct B {
    int x, y;
    /* Target ctor — the one we delegate to. */
    B(int xv, int yv) : x(xv), y(yv) { target_count++; }
    /* Delegating ctor — must call B(int, int) on `this`. */
    B(int xv) : B(xv, xv * 2) {}
};

int main() {
    B b(7);
    if (target_count != 1) abort();
    if (b.x != 7 || b.y != 14) abort();
    return 0;
}
