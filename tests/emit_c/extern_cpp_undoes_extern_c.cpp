// EXPECT: 0
// N4659 §10.5 [dcl.link] — `extern "C++"` explicitly selects C++
// linkage and undoes any enclosing `extern "C"` for the nested
// declarations. System headers do this routinely, e.g.
// <bits/iscanonical.h> wraps three inline overloads of `iscanonical`
// (float / double / long double) in `extern "C++" { ... }` inside
// the surrounding `extern "C"` block from <math.h>.
//
// Without honoring the inner `extern "C++"`, sea-front marked all
// three overloads as C-linkage and the dedup-on-c-linkage path kept
// only the first decl, then the C compiler errored on the remaining
// two (conflicting types for the bare 'iscanonical' name).
extern "C" {
    extern int __iscanonicall(long double);
    extern "C++" {
        inline int iscanonical(float v)       { (void)v; return 1; }
        inline int iscanonical(double v)      { (void)v; return 1; }
        inline int iscanonical(long double v) { return __iscanonicall(v); }
    }
}

int main() { float f = 1.0f; return iscanonical(f) ? 0 : 1; }
