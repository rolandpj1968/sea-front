// EXPECT: 0
// N4659 §16.3.1.5 [over.match.conv]: a class with `operator T()
// const` defines an implicit conversion. When a non-class
// variable is initialized from a class-typed expression whose
// type carries such an operator, the converting member function
// is called.
//
// Sea-front's parser accepts `operator T()` member syntax and the
// mangler (`mangle_class_conversion`) already produces the
// Itanium `cv<T>` form, but no call-site dispatch existed — bare
// `int x = c;` reached cc as a type-incompatible init. The
// var-decl init path now looks up the matching conversion op on
// the rhs class and emits the call.
//
// Filter: conversion ops take zero explicit arguments. Without
// this, the finder would match unrelated members like
// `int operator-(const T&)` whose int return type isn't a
// conversion.

extern "C" void abort();

int int_calls = 0;
int bool_calls = 0;

struct Counter {
    int v;
    Counter(int x) : v(x) {}
    operator int() const { ++int_calls; return v; }
    operator bool() const { ++bool_calls; return v != 0; }
};

int main() {
    Counter c(42);

    /* Simple int target. */
    int x = c;
    if (x != 42) abort();
    if (int_calls != 1) abort();

    /* Bool target picks the bool conversion. */
    bool b = c;
    if (!b) abort();
    if (bool_calls != 1) abort();

    /* Zero value, bool conversion returns false. */
    Counter z(0);
    bool zb = z;
    if (zb) abort();
    if (bool_calls != 2) abort();

    return 0;
}
