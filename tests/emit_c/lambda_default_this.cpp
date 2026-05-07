// EXPECT: 21
// '[&]' inside a non-static method picks up *this implicitly when
// the body references a class member (§8.1.5.2/8). The walker
// sees 'v' resolves through REGION_CLASS and synthesises a
// '[this]' capture entry.

struct Foo {
    int v;
    int run() {
        auto f = [&]() -> int { return v * 3; };
        return f();
    }
};
int main() {
    Foo foo;
    foo.v = 7;
    return foo.run();
}
