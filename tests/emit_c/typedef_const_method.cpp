// EXPECT: 0
// `typedef void Fn() const;` carries the const-method qualifier
// (N4659 §10.1.7.1 [dcl.type.cv] + §11.3.5/4 [dcl.fct]) on the
// TY_FUNC. When `Fn fn;` declares a class member, the typedef's
// is_const must survive — the OOL definition `void Foo::fn() const`
// mangles its symbol with `K`, and the call site mangle must agree
// or the link fails.
//
// Pre-fix: parse_type_specifiers copied the typedef's Type but
// overwrote is_const with the use-site spec (false here), so the
// forward decl and call site mangled as non-const while the def
// mangled as const → undefined reference.

extern "C" void abort(void);

typedef void Fn() const;

struct Foo {
    int x;
    Fn fn;
};

int g_called = 0;
void Foo::fn() const { ++g_called; }

int main() {
    Foo f;
    f.fn();
    if (g_called != 1) abort();
    return 0;
}
