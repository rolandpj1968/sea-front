// EXPECT: 42
// Binary operator on a hoisted struct-returning call:
//   o = o.and_not(m) | i;
// The hoist materializes 'o.and_not(m)' as __SF_temp_0 so the outer
// '|' can take its address. Operator resolution then runs against
// __SF_temp_0's Type — which, for a class whose class_def is unhooked
// on the return-type Type copy, used to miss the candidate and emit
// an unmangled 'sf__T__bitor(...)' with no param suffix. The emitted
// symbol doesn't match any definition ⇒ 'incompatible types' at the
// C compiler. collect_operator_candidates now follows the same
// class_region->owner_type->class_def fallback as the method-lookup
// path. Pattern: gcc 4.8 combine.c record_promoted_value.

struct Val {
    int v;
    Val bitor_(const Val &o) const { Val r; r.v = v | o.v; return r; }
    Val operator|(const Val &o) const { return bitor_(o); }
};

static Val make(int x) { Val r; r.v = x; return r; }

int main() {
    Val a; a.v = 40;
    Val b; b.v = 2;
    Val c = make(a.v) | b;   // lhs is an rvalue call — hoisted, then '|' dispatched
    return c.v;
}
