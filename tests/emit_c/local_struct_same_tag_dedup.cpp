// EXPECT: 0
// Two function-bodies each declare their OWN struct with the same
// source tag — distinct types in C++ (per-function-body scope, N4659
// §9.8 [basic.scope]). When sea-front hoists local classes to the
// TU it must dedup byte-identical bodies, else the C compiler sees
// `struct sf__Pair { ... }; struct sf__Pair { ... };` and rejects
// the redefinition.
//
// Real-world hit: gcc 4.8 config/i386/i386.c declares
// `_function_version_info` inside BOTH feature_compare() and
// dispatch_function_versions() with byte-identical fields.

extern "C" void abort();

int caller_a() {
    typedef struct Pair { int first; int second; } Pair;
    Pair p;
    p.first = 3;
    p.second = 4;
    return p.first * p.second;
}

int caller_b() {
    struct Pair { int first; int second; } *q;
    Pair r;
    r.first = 5;
    r.second = 6;
    q = &r;
    return q->first + q->second;
}

int main() {
    if (caller_a() != 12) abort();
    if (caller_b() != 11) abort();
    return 0;
}
