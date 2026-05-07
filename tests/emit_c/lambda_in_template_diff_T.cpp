// EXPECT: 103
// Two distinct instantiations of the same lambda-bearing template
// (T=int, T=long). Each instantiation gets its own closure
// '__sf_closure_<N>_instK' tag and fn '__sf_lambda_<N>_instK_*'
// so the symbols don't collide at link time.

template<typename T>
T pick(T a, T b) {
    auto f = [a, b]() -> T { return a < b ? a : b; };
    return f();
}
int main() {
    int  i = pick<int>(7, 3);
    long l = pick<long>(100L, 200L);
    return (int)(i + l);
}
