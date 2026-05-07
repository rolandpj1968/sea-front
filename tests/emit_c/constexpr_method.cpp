// EXPECT: 42
// 'constexpr' member function — N4659 §10.1.5/1: implicitly inline.
// Methods are emitted via the class-method path (with a 'this' param);
// the constexpr → inline upgrade rides on func.storage_flags through
// emit_func_def's existing path.
struct Foo {
    int v;
    constexpr int doubled() const { return v * 2; }
};
int main() {
    Foo foo;
    foo.v = 21;
    return foo.doubled();
}
