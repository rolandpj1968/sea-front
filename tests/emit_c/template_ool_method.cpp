// EXPECT: 42
template<typename T>
struct Box {
    T val;
    T get();
    void set(T v);
};

template<typename T>
T Box<T>::get() { return val; }

template<typename T>
void Box<T>::set(T v) { val = v; }

int main() {
    Box<int> b;
    b.set(42);
    return b.get();
}
