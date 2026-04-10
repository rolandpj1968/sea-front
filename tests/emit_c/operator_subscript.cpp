// EXPECT: 42
struct Vec {
    int buf[8];
    int len;
    int operator[](int i) { return buf[i]; }
};

int main() {
    Vec v;
    v.buf[0] = 10;
    v.buf[1] = 20;
    v.buf[2] = 5;
    v.buf[3] = 7;
    v.len = 4;
    return v[0] + v[1] + v[2] + v[3];
}
