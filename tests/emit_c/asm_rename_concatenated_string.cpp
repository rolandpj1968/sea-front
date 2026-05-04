// EXPECT: 42
// GCC __asm("name") declarator-suffix selects the C ABI symbol the
// declared function binds to. The argument follows C/C++ adjacent
// string-literal concatenation (N4659 §5.13.5/13 [lex.string]):
//   void f() __asm__("" "__real_f");
// concatenates to "__real_f". Sea-front previously captured only the
// FIRST string literal, so an empty-string lead trick made the asm-
// rename emit NOTHING and the call site became a comma expression.
//
// Real-world bite: glibc <stdlib.h>'s
//   strtoul (...) __asm__ ("" "__isoc23_strtoul")
// turned 'opnum = strtoul(p, &e, 10)' into 'opnum = (p, &e, 10)' —
// opnum became 10, indexing past valid operands and segfaulting cc1plus.

extern "C" unsigned long real_strtoul(const char *, char **, int);

extern "C" unsigned long fancy(const char *, char **, int)
    __asm__("" "real_strtoul");

unsigned long real_strtoul(const char *s, char **endp, int base) {
    (void)endp; (void)base;
    return (unsigned long)(s[0] - '0') * 10 + (unsigned long)(s[1] - '0');
}

int main() {
    char *e;
    return (int)fancy("42", &e, 10);   // 4*10+2 = 42
}
