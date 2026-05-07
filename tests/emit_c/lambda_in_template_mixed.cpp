// EXPECT: 42
// Mixed capture inside a template: '&x' (by-ref) and 'y' (by-value).
// Both captures' types depend on T and get substituted at clone time.

template<typename T>
T apply(T x, T y) {
    auto f = [&x, y]() -> T { return x + y; };
    return f();
}
int main() {
    return apply<int>(10, 32);
}
