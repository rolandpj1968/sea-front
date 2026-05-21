// EXPECT: 0
// __PRETTY_FUNCTION__ must render `typename T::type` (a qualified
// dependent type) in BOTH the signature and the `[with ...]`
// substitution suffix. N4659 §13.8.3 [temp.dep.type] — the
// printer keeps the source form `typename T::member` until
// instantiation resolves T to a concrete type, then lists the
// resolved member type alongside the bound T.
//
// Without the qualifier handling, sea-front previously printed
// the second param as just "T" and dropped the resolved member
// substitution. Pattern: g++.dg/diagnostic/bindings1.C.

extern "C" int strcmp(const char *, const char *);

template <typename T>
const char *foo(T, typename T::type) { return __PRETTY_FUNCTION__; }

struct X { typedef int type; };

int main() {
    const char *s = foo(X(), 7);
    const char *expected =
        "const char* foo(T, typename T::type) "
        "[with T = X; typename T::type = int]";
    return strcmp(s, expected) == 0 ? 0 : 1;
}
