// EXPECT: 1
// Enum body uses C++ keywords 'true' / 'false' as enumerator
// initializers. emit_c must substitute them with 1/0 since the
// emitted C doesn't include <stdbool.h>. Pattern from
// gcc 4.8 cp/semantics.c.

int main() {
    enum { any = false, rval = true };
    return rval - any;  // 1 - 0 == 1
}
