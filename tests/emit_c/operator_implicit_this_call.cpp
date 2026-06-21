// EXPECT: 0
// Unqualified `operator==(arg)` call inside another method body —
// the parser stores the callee as ND_IDENT named just "operator"
// (8 chars, operator-symbol survives via chars after token->loc+len).
// Sema doesn't recognise it as implicit-this, leaving the codegen
// to emit a literal `operator(arg)` C symbol that doesn't exist.
//
// Fixed by two stitched pieces:
//   1. emit_c.c — recognise this shape inside a method body, promote
//      to implicit-this dispatch with g_current_method_class as the
//      receiver type.
//   2. mangle_itanium.c — itan_mangle_class_method_tid checks if the
//      method name is "operator…" and routes through the Itanium
//      op-id (eq, ne, lt, …) instead of emitting `8operator`.
//
// Real-world hit: libstdc++ <typeinfo>
//   bool operator!=(const type_info& __arg) const noexcept
//   { return !operator==(__arg); }

extern "C" void abort();

struct S {
    int v;
    bool operator==(const S& o) const { return v == o.v; }
    bool operator!=(const S& o) const { return !operator==(o); }
};

int main() {
    S a; a.v = 7;
    S b; b.v = 7;
    S c; c.v = 8;
    if (!(a == b)) abort();
    if (a != b) abort();
    if (!(a != c)) abort();
    return 0;
}
