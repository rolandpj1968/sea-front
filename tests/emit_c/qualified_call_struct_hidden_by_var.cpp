// EXPECT: 0
// A static variable can shadow a class name in the same scope:
//   struct Foo { ... };
//   static SomeOther Foo;        // shadows the struct name
// In the qualified-id `Foo::method`, lookup of `Foo` is class-name
// lookup — the variable shadow must be ignored. Without that, sea-
// front's visit_qualified resolved the leading qualifier to the
// variable's TYPE, codegen mangled the call with the variable's tag
// instead of the struct's, and link broke.
//
// Pattern from gcc 4.8 tree-ssa-threadupdate.c:
//   struct redirection_data : ... { static hashval_t hash(...); };
//   static hash_table<redirection_data> redirection_data;   // shadow
//   ...
//   redirection_data::hash(p);   // <-- needs to call the struct method
//
// N4659 §6.4.3 [basic.lookup.qual] / §6.3.10 [basic.scope.hiding].

struct Box {
    int v_;
    static int peek(int x) { return x; }
};

struct Holder {
    int data_;
};

static Holder Box;        // variable shadows the struct name in this scope.

int main() {
    return Box::peek(0); // class-name lookup: ignore the variable, find struct's static.
}
