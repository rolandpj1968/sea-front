// EXPECT: 0
// `new (T)` — the parenthesised-type-id form of a new-expression.
// N4659 §8.3.4/1 [expr.new]:
//   new-expression: ::opt new new-placement(opt) new-type-id
//                                              new-initializer(opt)
//                 | ::opt new new-placement(opt) ( type-id )
//                                              new-initializer(opt)
//
// Sea-front always parsed `new (...)` as the placement-args form,
// then failed when no type-id followed the closing paren. The
// disambiguation can only happen AFTER the parens: if what follows
// can't start a new-type-id (`;`, `,`, `)`, `}`, `?`, `:`, EOF),
// the parens hold the type-id itself.
//
// Fix: after the tentative placement-arg parse succeeds, peek what
// follows; if it's a clear new-expression terminator, rewind and
// re-parse the parens as a type-id with no placement args.
//
// Reduced from g++.dg/init/new11.C `new (X);`.

extern "C" void abort();
extern "C" void *malloc(unsigned long);
extern "C" void  free(void *);

int dtor_count = 0;
struct X { int v; X() : v(7) {} ~X() { ++dtor_count; } };

int main() {
    X *p1 = new (X);    /* parens-around-type-id form */
    if (!p1) abort();
    if (p1->v != 7) abort();
    delete p1;
    if (dtor_count != 1) abort();
    return 0;
}
