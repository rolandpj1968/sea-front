// EXPECT: 42
// Comprehensive: namespaced templates, nested template types,
// defaults with template-id, function templates, method calls
namespace lib {
    template<typename T>
    struct holder {
        T val;
        T get() { return val; }
        void set(T v) { val = v; }
    };

    template<typename K, typename V>
    struct pair {
        K first;
        V second;
    };

    template<typename T, typename Alloc = holder<T> >
    struct container {
        Alloc storage;
        int count;
        int size() { return count; }
    };
}

template<typename T>
T add(T a, T b) { return a + b; }

int main() {
    lib::holder<int> h;
    h.set(10);

    lib::pair<int, long> p;
    p.first = h.get();
    p.second = 32;

    lib::container<int> c;
    c.count = add<int>(p.first, (int)p.second);

    return c.size();
}
