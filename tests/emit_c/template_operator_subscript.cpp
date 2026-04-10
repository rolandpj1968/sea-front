// EXPECT: 42
template<typename T>
struct Array {
    T buf[8];
    int len;
    T operator[](int i) { return buf[i]; }
};

int main() {
    Array<int> a;
    a.buf[0] = 10;
    a.buf[1] = 20;
    a.buf[2] = 5;
    a.buf[3] = 7;
    a.len = 4;
    return a[0] + a[1] + a[2] + a[3];
}
