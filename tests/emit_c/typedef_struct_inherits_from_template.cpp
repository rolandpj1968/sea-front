// EXPECT: 42
// A struct defined inside a 'typedef struct ... *Alias' wrapper that
// INHERITS from a class template instantiation. The base supplies a
// static method that the derived class should resolve through.
//
// Pattern from gcc 4.8 tree-ssa-pre.c:
//   typedef struct pre_expr_d : typed_noop_remove<pre_expr_d>
//   { ... } *pre_expr;
// hash_table<pre_expr_d>::dispose's cloned body calls
// pre_expr_d::remove(p) which must mangle through
// typed_noop_remove<pre_expr_d>.
//
// The bug this guards against: post-instantiation base-region
// patching only walked top-level ND_CLASS_DEFs, missing class
// definitions wrapped inside ND_TYPEDEF nodes. Without the patch
// the derived class's class_region had nbases=0 and the codegen
// base-walk in qualified-call mangling didn't find the inherited
// method's owner.
//
// Standard: N4659 §13.1 [class.derived] +
// §10.1.3 [dcl.typedef] (typedef of struct).

template<typename T>
struct NoopRemove {
    static int remove(T* p) { return 42; }
};

typedef struct Item : NoopRemove<struct Item>
{
    int x;
} *ItemPtr;

template<typename Descriptor>
struct Container {
    int do_remove(Item* p) {
        return Descriptor::remove(p);
    }
};

int main() {
    Container<Item> c;
    Item it;
    return c.do_remove(&it);
}
