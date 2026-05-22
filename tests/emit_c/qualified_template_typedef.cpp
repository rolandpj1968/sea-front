// EXPECT: 0
// `S<T>::type` — qualified-name access to a typedef inside an
// instantiated class template. N4659 §17.7.2 [temp.point]: the
// template is instantiated lazily at first use; sea-front
// resolves the typedef by substituting the template-id's
// arguments into the typedef target's dependent type. Without
// this, sea-front emitted the entire instantiated class as the
// type, so `sizeof(S<int>::type)` returned sizeof(S<int>) instead
// of sizeof(int).
//
// Pattern: g++.dg/ext/tmplattr5.C — the test compares sizeof of
// a member typedef against sizeof of a global typedef. Also the
// STL workhorse `Container::value_type` / `Container::iterator`.

template <typename T>
struct Box {
    typedef T value_type;
    typedef T * pointer;
};

template <typename T>
struct Pair {
    typedef T first_type;
    T first;
    T second;
};

int main() {
    // Scalar typedef resolution.
    Box<int>::value_type x = 42;
    if (x != 42) return 1;
    if (sizeof(Box<int>::value_type) != sizeof(int)) return 2;

    // Pointer typedef resolution.
    int y = 7;
    Box<int>::pointer p = &y;
    if (*p != 7) return 3;
    if (sizeof(Box<int>::pointer) != sizeof(int *)) return 4;

    // Across two template params.
    Pair<double>::first_type d = 3.14;
    if (sizeof(Pair<double>::first_type) != sizeof(double)) return 5;
    if (d != 3.14) return 6;

    return 0;
}
