// EXPECT: 0
// Function-try-block — N4659 §15.4 [except.handle]:
//   function-body: function-try-block
//   function-try-block: try compound-statement handler-seq
// The whole function body is wrapped in a try-block; equivalent to
//   void f() { try { body } catch (...) { handler } }
// for ordinary (non-ctor/non-dtor) functions.
//
// Note: in ctor/dtor function-try-blocks the standard requires
// member/base destruction to run BEFORE entering the catch handler
// (since the handler exists to react to a failure in the
// construction or destruction). That semantic isn't yet wired up —
// this test covers only the regular-function form.

extern "C" void abort();

int g_caught = 0;

int divide(int n, int d)
try {
    if (d == 0) throw 0;
    return n / d;
}
catch (int) {
    g_caught++;
    return -1;
}

int main() {
    if (divide(10, 2) != 5) abort();
    if (g_caught != 0) abort();
    if (divide(10, 0) != -1) abort();
    if (g_caught != 1) abort();
    return 0;
}
