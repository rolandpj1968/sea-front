// EXPECT: 110
// Same class name in two different namespaces — without namespace
// mangling these would collide as a single C 'struct Thing'. With
// the prefix they become 'a_Thing' and 'b_Thing'.
namespace a {
    struct Thing {
        int v;
        int get() { return v; }
    };
}
namespace b {
    struct Thing {
        int v;
        int get() { return v + 100; }
    };
}

int main() {
    a::Thing x;
    x.v = 5;
    b::Thing y;
    y.v = 5;
    return x.get() + y.get();
}
