// EXPECT: 0
// __has_nothrow_copy(T) is a GCC type trait that returns true iff
// T's copy ctor is known not to throw. Pre-fix sea-front returned
// 0 for any class with a user-declared ctor — too strict.
//
// The correct rule (N4659 §15.8.1 [class.copy.ctor] + §18.4
// [except.spec]):
//   - Look for a user-declared copy ctor (single ref-to-T param,
//     non-template).
//   - If found, return its `throw()` / `noexcept` flag.
//   - If not found, the implicit copy ctor is generated and is
//     conservatively non-throwing.
// Template ctors are NEVER copy ctors (N4659 §15.8.1/2).
//
// Pattern: g++.dg/ext/has_nothrow_copy-{2,3,...}.C.

extern "C" void abort();

// User copy ctor with throw() — should be nothrow.
struct A {
    A(const A&) throw() {}
};

// User copy ctor without throw spec — sea-front conservatively
// says not nothrow.
struct B {
    B(const B&) {}
};

// No user copy ctor, only a template ctor. Implicit copy ctor
// applies; should be nothrow.
struct C {
    template <class T> C(T) throw(int) {}
};

// User copy ctor + template ctor — copy ctor wins, has throw().
struct D {
    D(const D&) throw() {}
    template <class T> D(T) throw(int) {}
};

int main() {
    if (!__has_nothrow_copy(A)) return 1;
    if ( __has_nothrow_copy(B)) return 2;
    if (!__has_nothrow_copy(C)) return 3;
    if (!__has_nothrow_copy(D)) return 4;
    return 0;
}
