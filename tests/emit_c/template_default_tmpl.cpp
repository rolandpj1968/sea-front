// EXPECT: 52
// Default template argument that itself is a template instantiation
template<typename T>
struct allocator {
    T *ptr;
    int n;
};

template<typename T, typename Alloc = allocator<T> >
struct container {
    Alloc alloc;
    int size;
};

int main() {
    container<int> c;
    c.size = 42;
    container<double> d;
    d.size = 10;
    return c.size + d.size;
}
