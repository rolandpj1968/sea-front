// EXPECT: 50
// Qualified static-method call chained with instance method:
// 'X::make(n).get()'. The inner ND_CALL's callee is ND_QUALIFIED
// (not a plain ND_IDENT), so the returned TY_STRUCT came through
// a different sema path and landed with class_region set but
// class_def null. collect_overload_candidates bailed out, so the
// outer '.get()' dispatched as a C field access (wrong). Fixed by
// falling back through class_region->owner_type->class_def.
struct X {
    int v;
    static X make(int val) { X x; x.v = val; return x; }
    int get() const { return v; }
};

int main() {
    return X::make(50).get();
}
