// EXPECT: 12
// Same as method_calls_method.cpp but with the methods declared in
// the OPPOSITE order — quadrupled() before doubled(). Without forward
// declarations, the C compiler would warn (or fail) because
// Box_doubled is referenced before it's declared. emit_class_def
// emits a forward-decl pass for all methods first.
struct Box {
    int v;
    int quadrupled() { return doubled() + doubled(); }
    int doubled() { return v + v; }
};

int main() {
    Box b;
    b.v = 3;
    return b.quadrupled();
}
