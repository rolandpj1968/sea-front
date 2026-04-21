// EXPECT: 42
// C++ allows two free functions with the same name but different
// signatures (overloading). C does not. When sea-front emits unmangled
// free functions, colliding overloads in the same TU produce
// 'conflicting types' errors at the C compiler.
//
// For the dedup path: a PRIOR extern declaration with one signature
// followed by a static-inline DEFINITION with a different signature
// (different struct tag at the first pointer parameter) must be
// detected and the later definition skipped — the file likely never
// calls the skipped overload; if it does, the link will fail cleanly.
//
// Pattern from gcc 4.8 bitmap.h / sbitmap.h:
//   extern bool bitmap_bit_p (bitmap_head_def*, int);   // bitmap.h
//   static inline ulong bitmap_bit_p (simple_bitmap_def*, int); // sbitmap.h
// The TU never uses the sbitmap overload but includes both headers.

struct Foo { int v; };
struct Bar { int v; };

extern int helper(Foo* f, int n);
static inline long helper(Bar* b, int n) { return b->v + n; }

int helper(Foo* f, int n) { return f->v + n; }

int main() {
    Foo f; f.v = 40;
    return helper(&f, 2);  // 40 + 2 = 42 via the Foo overload
}
