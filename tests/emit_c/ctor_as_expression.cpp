// EXPECT: 8
// Constructor as an expression: 'Foo(7)' inside another
// expression produces a temporary of type Foo. The temp is
// hoisted via D-Hoist (now extended to recognize ctor-call
// shape) and materialized as:
//
//   struct Foo __SF_temp_0;
//   Foo_ctor(&__SF_temp_0, 7);
//   ... use(__SF_temp_0) ...
//   __SF_cleanup_0: ;
//   Foo_dtor(&__SF_temp_0);
//
// Two distinct pieces of work compose here:
//   - sema visit_call recognizes 'Foo(7)' as a ctor temp when
//     the callee identifier resolves to a type-name (ENTITY_TYPE
//     or ENTITY_TAG); sets call->resolved_type to the class.
//   - hoist_emit_decl detects the same callee shape and emits
//     the two-line construction form (separate decl + ctor call)
//     instead of the bare assignment-init that works for
//     class-RETURNING function calls.
//
// use() takes Foo by value, gets v=7, returns 7. Then ~Foo
// fires (g=1), and the outer 'return x + g' reads x=7 and g=1.
int g = 0;

struct Foo {
    int v;
    Foo(int x) { v = x; }
    ~Foo() { g = g + 1; }
};

int use(Foo f) { return f.v; }

int main() {
    int x = use(Foo(7));
    return x + g;
}
