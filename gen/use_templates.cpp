// Template instantiation showcase for sea-front
// Exercises: class templates, function templates, namespaced templates,
// nested templates, default arguments, multiple instantiations

namespace util {
    template<typename T>
    struct holder {
        T value;
        T get() { return value; }
        void set(T v) { value = v; }
    };
}

template<typename K, typename V>
struct entry {
    K key;
    V val;
};

template<typename T, typename Store = util::holder<T> >
struct cached {
    Store store;
    int valid;
    void put(T v) { store.set(v); valid = 1; }
    T fetch() { return store.get(); }
};

template<typename T>
T max_of(T a, T b) {
    if (a > b) return a;
    return b;
}

int main() {
    // Direct template class
    entry<int, long> e;
    e.key = 1;
    e.val = 100;

    // Namespaced template
    util::holder<int> h;
    h.set(20);

    // Template with default using another template
    cached<int> c;
    c.put(22);

    // Function template
    int m = max_of<int>(h.get(), (int)e.val);

    // Multiple instantiations
    cached<long> c2;
    c2.put(0);

    return e.key + (int)e.val + h.get() + c.fetch() - m;
    // 1 + 100 + 20 + 22 - 100 = 43... hmm
}
