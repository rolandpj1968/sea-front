// Half of the multi-TU dedup test. This file and inline_dedup_b.cpp
// both define the same class textually (we don't have an #include
// pipeline yet; this stands in for "both files include the same
// header"). Both produce a Counter_ctor / Counter_dtor / Counter_get
// in their lowered C, all marked __SF_INLINE. The linker dedupes
// to one survivor each.

struct Counter {
    int n;
    Counter() { n = 0; }
    void inc() { n = n + 1; }
    int get() { return n; }
};

int run_a() {
    Counter c;
    c.inc();
    c.inc();
    return c.get();   // 2
}
