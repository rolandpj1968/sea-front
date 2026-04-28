// EXPECT: 7
// resolve_free_function_overload's tiebreak fallback (last-resort
// "pick viable[0]" when no candidate strictly dominates the others)
// previously returned the winner Decl WITHOUT setting
// out_is_template / out_deduced. visit_call then thought the
// winner was a non-template, skipped the ND_TEMPLATE_ID rewrite,
// and the call site emitted the bare un-instantiated template
// name. Link failed at the bare name (no def emitted).
//
// Pattern reproduces when two function-template overloads have
// near-identical signatures so neither dominates: gcc 4.8 vec_free
// hit this with 49 unresolved refs in cc1plus link.
template<typename T>
struct Box { T data; };

// Two function-template overloads. Both are class-templates of
// Box, both 1-arg, both share the same param structure modulo
// the inner template arg. Sema's overload resolution sees both
// viable for 'process(b)' where b is a Box<T> — neither
// dominates, so the tiebreak fallback fires.
template<typename T>
int process(Box<T> *p) { return p->data; }

template<typename T>
int process(Box<T> &r) { return r.data; }

int main() {
    Box<int> b;
    b.data = 7;
    return process(&b);   // picks pointer overload, deduces T=int
}
