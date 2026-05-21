// EXPECT: 0
// Array of class-with-dtor: dtor fires per element at scope exit —
// N4659 §15.6.2/12 [class.base.init].
//
// Regression for the array-of-class dtor loop (b727199). Two shapes:
//   - Sized array `C arr[3]` — known length, reverse-order loop.
//   - Empty-struct elem — gcc extension makes sizeof(elem)==0; the
//     codegen's fallback `sizeof(arr)/sizeof(arr[0])` would trip a
//     compile-time div-by-zero trap without the guard.
//
// We can't verify reverse-construction order via per-element idx
// because sea-front doesn't yet ctor-loop each array element on
// default-init (that's a separate slice). We do verify that the
// dtor fires exactly once per element.

int c_dtor_count = 0;
int empty_dtor_count = 0;

struct C { ~C() { ++c_dtor_count; } };
struct Empty { ~Empty() { ++empty_dtor_count; } };

int main() {
    {
        C arr[3];
    }
    if (c_dtor_count != 3) return 1;

    {
        Empty e[2];
    }
    if (empty_dtor_count != 2) return 2;

    return 0;
}
