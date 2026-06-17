// EXPECT: 0
// GNU `#pragma pack(N)` packs subsequent struct/union/class defs to
// N-byte alignment until `#pragma pack()` resets to default. The
// pragma must affect only types whose `struct`/`union`/`class`
// keyword is on a line at or after the pragma's source line; the
// enclosing struct (started before the pragma) is unaffected.
//
// Reduced from g++.dg/parse/pragma3.C.

extern "C" void abort(void);

struct S
{
    char a[3];
#pragma pack(1) /* A block comment
                   that ends on the next line.  */
    struct T
    {
        char b;
        int c;
    } d;
#pragma pack /*/ */ () // C++ comment
    int e;
} s;

int main() {
    /* T is packed: char (1) + int (4) = 5 bytes. */
    if (sizeof(int) == 4 && sizeof(S::T) != 5) abort();
    /* S is NOT packed: char[3] (3) + pad (1) + T (5) + pad (3) + int (4) = 16,
     * but T being packed to 5 bytes means alignment matters here too.
     * gcc lays it out as: a[3]=3, padding=0, T(packed)=5, padding=0, e=4 — total 12. */
    if (sizeof(int) == 4 && sizeof(s) != 12) abort();
    return 0;
}
