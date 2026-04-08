// EXPECT: 5
// Driver for the multi-TU dedup test. Calls run_a() and run_b()
// (defined in inline_dedup_a.cpp and inline_dedup_b.cpp), expects
// 2 + 3 = 5.
//
// The success criterion is that all three TUs LINK together
// without "multiple definition" errors on Counter_ctor /
// Counter_dtor / Counter_inc / Counter_get. Without __SF_INLINE
// the link would fail because each TU emits its own copy of
// every Counter method.

int run_a();
int run_b();

int main() {
    return run_a() + run_b();
}
