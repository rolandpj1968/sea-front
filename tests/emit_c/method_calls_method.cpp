// EXPECT: 12
// Method calling another method on the same instance.
// 'doubled()' inside 'quadrupled()' is parsed as (call (ident "doubled"))
// where the ident is implicit-this. Codegen recognises this shape and
// emits 'Box_doubled(this)' instead of 'this->doubled()'.
struct Box {
    int v;
    int doubled() { return v + v; }
    int quadrupled() { return doubled() + doubled(); }
};

int main() {
    Box b;
    b.v = 3;
    return b.quadrupled();
}
