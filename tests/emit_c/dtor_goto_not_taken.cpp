// EXPECT: 7
// Negative path for the goto-across-cleanup fix: the if-condition
// is FALSE, so the goto isn't taken. Control falls through, and
// ~t fires via the normal cleanup chain at end of inner block.
//
// The risk this test guards against: if the inline-dtor-then-goto
// shape weren't properly braced, an unbraced 'if (cond) goto X;'
// would emit 'if (cond) T_dtor(&t); goto X;' — the if would only
// guard the dtor, the goto would be unconditional, and the
// cleanup chain would also fire ~t at end of block — DOUBLE-fire.
//
// With proper braces ({ T_dtor(&t); goto X; }) the cond=false
// path skips the entire compound statement, control falls
// through to the cleanup chain, and ~t fires exactly once.
int g = 0;

struct T { int v; ~T() { g = g + v; } };

int main() {
    {
        T t;
        t.v = 7;
        if (t.v < 0) goto end;
    }
end: ;
    return g;
}
