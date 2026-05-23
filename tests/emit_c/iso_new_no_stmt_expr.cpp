// EXPECT: 7
// `new T()` should lower to ISO C statements + bare temp at use
// site — NOT a GNU statement-expression `({ T* t = malloc; ctor(t);
// t; })`. The stmt-expr form blocks ISO-only backends like cproc /
// QBE / sdcc / TCC.
//
// Verification: build the .c output with `gcc -std=c99 -pedantic
// -Wno-... -Werror` and ensure no stmt-expr appears. The runtime
// behaviour must match the stmt-expr form's semantics.

extern "C" void *malloc(unsigned long);
extern "C" void free(void *);

struct A {
    int x;
    A() : x(7) {}
};

int main() {
    A *p = new A();
    int r = p->x;
    free(p);
    return r;
}
