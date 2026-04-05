template<typename T>
T add(T a, T b) { return a + b; }

template<typename T, int N>
struct Array { T data[10]; };

template<typename T> struct Box { T val; };
template<typename A, typename B> struct Pair { A first; B second; };

int main() {
    int x = add<int>(1, 2);
    Array<int, 10> arr;
    Box<Box<int>> nested;
    Box<Pair<int, int>> complex;
    return x;
}
