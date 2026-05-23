// EXPECT: 0
// GCC __real__ / __imag__ unary operators narrow a complex value to
// its real / imaginary scalar component. Pre-fix sea-front aliased
// both keywords to TK_PLUS at lex time, so `__imag__ z` lowered to
// `+z` and stayed complex. The test below relies on the narrowing
// to fall out of the comparison correctly.
//
// Pattern: g++.dg/other/complex1.C.

extern "C" void abort();

struct C {
    __complex__ long double c;
};

int main() {
    C x = { 2 + 2i };

    int n = 1;
    C y = (n == 1) ? x : (C){ 3 + 3i };

    if (__imag__ y.c != 2) abort();
    if (__real__ y.c != 2) abort();
    return 0;
}
