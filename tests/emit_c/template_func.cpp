// EXPECT: 42
template<typename T>
T max_of(T a, T b) {
    if (a > b) return a;
    return b;
}

int main() {
    return max_of<int>(10, 42);
}
