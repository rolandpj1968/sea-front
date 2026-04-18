// EXPECT: 4
// Test: __builtin_offsetof inside a template method body with
// a local typedef for the class type. Pattern from vec.h:
//   typedef vec<T, A> Self;
//   return __builtin_offsetof(Self, data);
// The local typedef 'Self' must be resolvable, and the member
// 'data' must be preserved through cloning.
template<typename T>
struct Embed {
    int prefix;
    T data[1];
    static unsigned long embedded_size(unsigned n) {
        typedef Embed<T> Self;
        return __builtin_offsetof(Self, data) + n * sizeof(T);
    }
};

int main() {
    // offsetof(Embed<int>, data) should be sizeof(int) = 4
    // (prefix is int, data follows it)
    unsigned long off = Embed<int>::embedded_size(0);
    return (int)off;
}
