// EXPECT: 0
// N4659 §10.5 [dcl.link] — declarations inside extern "C" { ... }
// have C linkage and must NOT receive C++ name mangling. The
// emission must produce a symbol that resolves against libc / other
// C TUs at link time.
//
// The dual-declaration shape (char* and const char* overloads under
// extern "C") is technically ill-formed but appears in real headers
// (<cstring> declares both strchr overloads this way). Sea-front
// emits only the FIRST decl with that name; later mismatched-sig
// decls would clash on the bare C symbol.
//
// Surfaced when the gcc 4.8 cc1plus build began linking
// build/genmodes / gengtype / gengenrtl / genhooks against libc and
// found unresolved 'strchr_p_const_char_ptr_int_pe_' etc.
#include <string.h>

extern "C" {
    char *strchr(char *s, int c);
    const char *strchr(const char *s, int c);
}

int main() {
    const char *p = "hello";
    return strchr(p, 'e') == 0 ? 1 : 0;
}
