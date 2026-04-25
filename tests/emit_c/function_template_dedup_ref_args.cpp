// EXPECT: 30
// Multiple call sites for the same function-template instantiation
// where the param needs ref-arg adaptation (& wrapping). The FIRST
// call goes through the freshly-built TY_FUNC; subsequent calls go
// through the dedup-hit path, which previously didn't carry a
// TY_FUNC onto the rewritten call-site callee. Result: dedup'd call
// sites lost ref-arg adaptation and emitted 'fn(arg)' instead of
// 'fn(&arg)'.
//
// Pattern: gcc 4.8 coverage.c CONSTRUCTOR_APPEND_ELT — vec_safe_push
// is called repeatedly with the same T, and every call needs &v
// for the vec<T,A,vl_embed>*& param plus &obj for the const T& param.

template<typename T>
void put(T &slot, T value) { slot = value; }

int main() {
    int a = 0, b = 0, c = 0;
    put(a, 5);   // first instantiation — TY_FUNC built fresh
    put(b, 10);  // dedup hit — TY_FUNC must be carried over
    put(c, 15);  // dedup hit again
    return a + b + c;  // 5 + 10 + 15 = 30
}
