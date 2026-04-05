typedef int T;
T a(1);
T b(2 + 3);
int main() {
    T c(42);
    T d(a + b);
    return c + d;
}
