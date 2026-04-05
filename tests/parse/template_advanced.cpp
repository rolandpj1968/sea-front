template<typename T> struct Box { T val; };
template<typename A, typename B> struct Pair { A a; B b; };
template<typename T> T identity(T x) { return x; }
template<int N> struct Fixed { int data[10]; };
int main() {
    Box<Box<Box<int>>> deep;
    Pair<int, Box<int>> mixed;
    int x = identity<int>(42);
    Fixed<10> f;
    return x;
}
