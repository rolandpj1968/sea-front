// EXPECT: 42
// static_cast<T&&>(lvalue) is the lowering of std::move and the
// idiom for binding to an rvalue-ref parameter without invoking a
// move ctor. Sea-front lowers T&& as T*, so the cast must take the
// address of the lvalue, not cast the value bits to a pointer.
//
// N4659 §8.2.9 [expr.static.cast] / §8.3.2 [dcl.ref]. Earlier emit
// produced '(struct T*)x' which fails C type-checking when x is a
// struct value rather than a pointer.

struct Big { int data[8]; };

static int sink(Big&& b) { return b.data[0]; }

int main() {
    Big x; x.data[0] = 42;
    return sink(static_cast<Big&&>(x));
}
