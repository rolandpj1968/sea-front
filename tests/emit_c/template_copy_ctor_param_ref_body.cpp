// EXPECT: 0
// Regression for the implicit2 inline-body stopgap drop (commit af4f361).
// Before: emit_inline_copy_chain inlined the template ctor body when
// `template_body_is_inlineable` returned true — but the predicate
// rejected anything referencing the parameter (ND_MEMBER on `t`,
// sizeof(t), etc.) and fell back to the non-template copy ctor
// (empty body in this test → marker stays 0 → abort).
// After: synth_template_copy_ctors clones A<A>(A&) so the body runs
// through a real mangled call regardless of what it touches.
//
// Companion to copy_ctor_template_wins_overload.cpp (trivial body
// that WAS inlineable, passes pre and post fix) — this one only
// passes post-fix.

extern "C" void abort();

int marker = 0;

struct A {
    int seed;
    A() : seed(0) {}
    A(const A&) { /* empty — loses to template on cv-rank */ }
    template<class T> A(T& t) : seed(0) { marker = t.seed; }
};

struct B {
    B() {}
    B(B&) {}
};

struct C : A, B { };

int main() {
    C c;
    c.seed = 42;            /* set the A subobject's seed via inherited access */
    (void)C(c);             /* triggers C's synth copy ctor → A's template wins → marker = 42 */
    if (marker != 42) abort();
    return 0;
}
