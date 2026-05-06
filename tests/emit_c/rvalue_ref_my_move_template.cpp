// EXPECT: 42
// std::move replicated as a template wrapper. Inside the template
// body, the operand of 'static_cast<T&&>' is a T& parameter — already
// a pointer in our lowering. The cast must NOT take its address (that
// would yield T**); it should pass the bare pointer through.
//
// Companion to rvalue_ref_static_cast.cpp which exercises the
// value-operand path.

struct Big { int data[8]; };

template<typename T>
T&& my_move(T& x) { return static_cast<T&&>(x); }

static int sink(Big&& b) { return b.data[0]; }

int main() {
    Big x; x.data[0] = 7;
    return sink(my_move(x)) + 35;
}
