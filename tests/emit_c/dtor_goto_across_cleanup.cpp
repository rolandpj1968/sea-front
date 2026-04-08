// EXPECT: 7
// User 'goto' that jumps past a non-trivial local must fire the
// local's dtor on the way out (N4659 §15.4 [class.dtor]/6).
//
// Pre-fix sea-front had no codegen for ND_GOTO at all — both the
// goto and the label were emitted as '/* stmt */;' placeholders,
// which meant the program executed: ~t never fired (g stayed 0)
// and the goto never happened (control fell through and ~t fired
// via the cleanup chain). Coincidentally produced exit 7 by a
// completely different path.
//
// Post-fix the lowered C is:
//
//   if ((t.v > 0)) { T_dtor(&t); goto end; }
//
// Properly braced (so an unbraced 'if (cond) goto X;' stays
// correct), dtor inlined before the jump.
//
// Conservative approximation: ALL currently-live CL_VARs get
// their dtors fired before the goto, because we don't yet have
// sema-level label-depth tracking. Right for goto-to-function-
// scope (the common case); over-destroys for the rare case of
// jumps within the same scope. C++ already forbids gotos that
// jump INTO a scope past a non-trivial init, so the over-fire
// case mostly doesn't arise.
int g = 0;

struct T { int v; ~T() { g = g + v; } };

int main() {
    {
        T t;
        t.v = 7;
        if (t.v > 0) goto end;
    }
end: ;
    return g;
}
