// EXPECT: 0
// A local variable whose name matches a libc function in the
// free-function overload table (e.g. 'index' from <strings.h>,
// 'time' from <time.h>) must NOT be mangled. The mangle path
// runs only for resolved-to-a-function ident references; locals,
// struct fields, and any non-function ident emit bare.
//
// Surfaced by gcc 4.8 genautomata.c which has 'int index;' as a
// local; libstdc++'s <cstring> declares two overloads of 'index'
// under extern "C++" (the same dual-overload trick as strchr), so
// the overload table sees 'index' as having multiple sigs. Without
// the resolved-decl-is-function gate, the local got promoted to
// 'index_p_void_pe_', producing an "undeclared identifier" error
// at the C compile.
extern "C++" {
    char *index(char *s, int c) __asm("strchr");
    const char *index(const char *s, int c) __asm("strchr");
}

int main() {
    int index = 42;   // local shadowing the libc function
    return index - 42;
}
