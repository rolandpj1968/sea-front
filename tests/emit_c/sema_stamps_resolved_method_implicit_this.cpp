// EXPECT: 0
// Implicit-this method-call stamping: bare `method(args)` inside a
// method body. Sema picks the winner and stamps call.resolved_method;
// codegen reads the stamp instead of running resolve_overload_with_origin
// at emit time. N4659 §16.3 [over.match].
//
// Gates (sema-side): only on the post-instantiation visit pass (so
// template-body sema #1 doesn't stamp against incomplete bases); only
// when own_d is a direct declaration (no using-decl rerouting); only
// when no base of cur_class has a same-named method AND every base's
// class_region is reachable (so we can verify shadowing).

extern "C" void abort();

struct Box {
    int v;
    Box() : v(0) { }
    int compute(int x)         { return v + x; }
    int compute(int x, int y)  { return v + x * y; }
    int compute()              { return v + 100; }
    /* Each call is an implicit-this method call — sema picks the
     * matching compute() overload and stamps it on the call node.
     * Box has no bases so the base-shadow check trivially passes. */
    int via_one(int x)        { return compute(x); }
    int via_two(int x, int y) { return compute(x, y); }
    int via_zero()            { return compute(); }
};

int main() {
    Box b;
    b.v = 10;
    if (b.via_one(5)    != 15)  abort();
    if (b.via_two(3, 4) != 22)  abort();
    if (b.via_zero()    != 110) abort();
    return 0;
}
