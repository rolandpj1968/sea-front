// EXPECT: 0
// GNU __asm("symbol") declarator-suffix — non-standard (NOT N4659;
// the standard's §10.4 [dcl.asm] is for inline assembly declarations,
// NOT declarator-suffix renames). But glibc's <string.h> uses this
// idiom heavily inside extern "C++" { ... } to bind type-safe C++
// overloads to a single C ABI symbol (see /usr/include/string.h's
// strchr / strrchr / memchr).
//
// Sea-front captures the asm-label string and emits it as the symbol
// name at the decl AND at every call site, suppressing both the
// per-Declaration C++ mangling and the overload-disambiguation
// mangle. Multiple decls with the same asm-target collapse to a
// single emitted decl (extern-C dedup path).
//
// This test mirrors glibc's strchr pattern with a stub symbol so
// the link is self-contained.
extern "C" int my_strchr_impl(int x);

extern "C++" {
    int my_strchr(int x)       __asm("my_strchr_impl");
    int my_strchr(const int x) __asm("my_strchr_impl");
}

extern "C" int my_strchr_impl(int x) { return x == 7 ? 0 : 1; }

int main() {
    int x = 7;
    return my_strchr(x);
}
