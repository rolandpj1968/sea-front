// Half of the multi-TU dedup test. See inline_dedup_a.cpp for the
// shared class definition (textually duplicated since we have no
// header pipeline yet). Both files produce identical __SF_INLINE
// symbols; the linker keeps one copy.

struct Counter {
    int n;
    Counter() { n = 0; }
    void inc() { n = n + 1; }
    int get() { return n; }
};

int run_b() {
    Counter c;
    c.inc();
    c.inc();
    c.inc();
    return c.get();   // 3
}
