// EXPECT: 42
// '*this' as a primary expression — passing the current instance by
// value to another method that takes a Box parameter.
struct Box {
    int v;
    int copy_v(Box other) { return other.v; }
    int via_self() { return copy_v(*this); }
};

int main() {
    Box b;
    b.v = 42;
    return b.via_self();
}
