// EXPECT: 0
// p->func<int>() — member call with explicit method-template-args
// via `->`. The instantiation request created by collect_from_node's
// ND_CALL+ND_MEMBER path already passed member.template_id through;
// the inst pass at the explicit-args binding block recognised it.
// But deduce_template_args returns its `out->nentries > 0` check
// AFTER walking 0 deduction pairs (the called member has no
// function args), so it falsely returned 0 and the request was
// dropped — the body never emitted and the call linked to
// nothing.
//
// Fix: only bail on a deduce-false when the call had args (so
// deduction was the binding source). Zero-arg calls fall through
// to the explicit-args binding block.
//
// Distilled from g++.dg/template/non-dependent1.C (PR c++/8222).

extern "C" void abort();

struct Foo {
    template<typename T> T identity(T v) { return v; }
    template<typename> int marker() { return 42; }
};

template<typename> void Bar(Foo *p) {
    /* Zero-arg explicit-template-id call — the regression target. */
    if (p->marker<int>() != 42) abort();
    /* Sanity check the deduction path still works alongside. */
    if (p->identity<int>(7) != 7) abort();
}

int main() {
    Foo c;
    Bar<int>(&c);
    return 0;
}
