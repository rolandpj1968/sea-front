void swap(int &a, int &b) {
    int t = a;
    a = b;
    b = t;
}

int &&rval_ref(int &&x);
const int &cref = 42;

int main() {
    int x = 1, y = 2;
    swap(x, y);
    return x;
}
