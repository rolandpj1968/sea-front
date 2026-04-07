// EXPECT: 42
// Nested namespaces — mangled prefix must walk the full enclosing
// chain so the C tag becomes 'outer_inner_Box'.
namespace outer {
    namespace inner {
        struct Box {
            int v;
            int doubled() { return v + v; }
        };
    }
}

int main() {
    outer::inner::Box b;
    b.v = 21;
    return b.doubled();
}
