// EXPECT: 0
// Two related fixes exercised here:
//
//  - `::operator new(t)` from inside a user-defined `operator new[]`
//    must lower to the Itanium `_Znwm(t)` symbol, not to a bare
//    `operator(t)`. The qualified-id `::operator new` parses as
//    ND_QUALIFIED with global_scope=true, nparts=1, parts[0]=
//    "operator"; sea-front used to only catch the bare ND_IDENT
//    shape and miss this. N4659 §8.1.4.3 [expr.prim.id.qual].
//
//  - User-defined `operator new` and `operator new[]` must NOT
//    collide in the free-function dedup table. The bare "operator"
//    source token is reused for every operator variant — the
//    operator kind has to enter the dedup key. Without that, the
//    second def is silently dropped as "duplicate" and the symbol
//    is undefined at link time. N4659 §16.5 [over.oper].
//
// Reduced from g++.dg/cpp0x/defaulted19.C.

extern "C" void *malloc(unsigned long);

void *scalar_alloc_addr = 0;
void *array_alloc_addr = 0;

void *operator new(unsigned long t) {
    scalar_alloc_addr = malloc(t);
    return scalar_alloc_addr;
}

void *operator new[](unsigned long t) {
    array_alloc_addr = ::operator new(t);
    return array_alloc_addr;
}

struct A {};

int main() {
    A *ap = new A[5];
    if (ap != array_alloc_addr) return 1;
    if (array_alloc_addr != scalar_alloc_addr) return 2;
    return 0;
}
