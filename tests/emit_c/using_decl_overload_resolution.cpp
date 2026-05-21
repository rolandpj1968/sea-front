// EXPECT: 0
// `using Base::name;` inside a class brings the inherited method
// into the derived's name set; calls within the derived's methods
// resolve via overload resolution across ALL of the derived's
// (and its bases') methods with that name — N4659 §13.5.2
// [class.member.lookup] + §10.3.3 [namespace.udecl].
//
// Two diamonds (single-base inheritance with sibling bases):
//   - Foo::k(float) + Baz::k(int) — distinct kinds, exact match.
//   - Foo::k(double) + Baz::k(int) — distinct kinds with int arg
//     converting to double (float→int has the same shape).
//
// Sea-front's collect_overload_candidates already walks bases,
// but the dispatch path mangled with the LOOKUP ROOT class rather
// than the WINNER's class, so calls landed at non-existent symbols
// like `Bar::k`. Fix: track winner's origin class through
// resolve_overload, mangle with that. Plus: score_type_pair gained
// a "same arithmetic family" tier (rank 1) so float→float beats
// float→int when neither kind matches exactly.

int foo_k_calls = 0;
int baz_k_calls = 0;

struct Foo {
    int k(float)  { ++foo_k_calls; return 1; }
};

struct Baz {
    int k(int)    { ++baz_k_calls; return 2; }
};

struct Bar : Foo, Baz {
    using Foo::k;
    using Baz::k;
    int call_float() { return k(1.0f); }
    int call_int()   { return k(1); }
};

int main() {
    Bar bar;
    if (bar.call_float() != 1)         return 1;
    if (bar.call_int()   != 2)         return 2;
    if (foo_k_calls != 1)              return 3;
    if (baz_k_calls != 1)              return 4;
    return 0;
}
