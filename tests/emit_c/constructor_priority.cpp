// EXPECT: 0
// `__attribute__((constructor(N)))` / `((destructor(N)))` with an
// explicit priority. The attribute must round-trip through to the
// emitted C definition so the dynamic loader can order the
// constructors.
//
// Regression for 8591db7 (priority-arg parse/emit) and 9fa278f
// (bare ctor/dtor attribute pass-through). Single-TU check that
// the function runs before main and the priority arg doesn't
// break the symbol or pass-through.

int ctor_ran = 0;

__attribute__((constructor(150))) void early() {
    ctor_ran = 42;
}

int main() {
    return (ctor_ran == 42) ? 0 : 1;
}
