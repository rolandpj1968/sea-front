// EXPECT: 11
// Overload resolution between two free functions whose only parameter
// difference is a pointer to a different struct (both const-qualified
// at the pointee). The argument is a non-const pointer of one of the
// two struct tags, so resolving the call requires a §7.5 [conv.qual]
// qualification conversion (T* -> const T*) — but only against the
// matching-tag overload. Against the other-tag overload, the
// conversion is incompatible (different pointee aggregate type).
//
// Real-world shape: gcc 4.8 tree-into-ssa.c calling
//   bitmap_empty_p (db->def_blocks)
// where db->def_blocks is bitmap_head_def* and both
//   inline bool bitmap_empty_p(const bitmap_head_def *);   // bitmap.h
//   extern bool bitmap_empty_p(const simple_bitmap_def *); // sbitmap.h
// are in scope. Without struct-tag verification in the cv-conversion
// path, both candidates score QUAL_CONV equally and the picker ties
// to the first-declared overload — silently routing every call to
// the sbitmap overload, which then reads the bitmap_head_def's
// layout as if it were a simple_bitmap_def and corrupts gcc's
// SSA / PHI-insertion bookkeeping.

struct A { int a; };
struct B { int b; };

bool f(const A *p) { return p->a == 7;  }   // A-overload: peek field
bool f(const B *p) { return p->b == 99; }   // B-overload

int main() {
    A a = { 7 };
    A *pa = &a;       // non-const A* — needs T* -> const T* on the A-overload
    // If sea-front picks the B-overload, it reads p->b which aliases
    // a.a (both are first int), but compares against 99 not 7 — wrong.
    int n = f(pa) ? 11 : 22;
    return n;
}
