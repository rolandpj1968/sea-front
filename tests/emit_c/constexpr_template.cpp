// EXPECT: 42
// 'constexpr' on a function template — the constexpr propagates onto
// the instantiated function's storage_flags during cloning, so the
// lowered C definition emits as 'static inline' just like a non-
// template constexpr fn would.
template<typename T>
constexpr T cube(T x) { return x * x * x; }
int main() {
    return cube<int>(2) * 5 + 2;   // 8*5 + 2
}
