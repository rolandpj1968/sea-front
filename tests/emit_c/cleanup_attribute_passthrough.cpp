// EXPECT: 42
// GCC '__attribute__((cleanup(handler)))' on a local variable is a
// non-standard extension that gcc the back-end cc supports natively
// at the C level. Sea-front previously dropped the attribute when
// skipping GNU attributes — so the cleanup handler never ran.
//
// Capture the cleanup-handler identifier in the parser's attribute
// walker and re-emit the attribute on the C variable; gcc/cc then
// inserts the handler call at scope exit. Pattern verified by gcc
// 4.8 g++.dg/ext/cleanup-{2,4}.C and friends.

int log = 0;

extern "C" void handler(int *p) {
    log = *p + 42;
}

static void doit() {
    int x __attribute__((cleanup(handler))) = 0;
    (void)x;
}

int main() {
    doit();
    return log;          // handler ran → log = 0 + 42 = 42
}
