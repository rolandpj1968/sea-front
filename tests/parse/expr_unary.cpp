int main() {
    int x = 5;
    int a = -x;
    int b = +x;
    int c = !x;
    int d = ~x;
    int *p = &x;
    int e = *p;
    x++;
    x--;
    ++x;
    --x;
    int f = sizeof(int);
    int g = sizeof x;
    return a + b + c + d + e + f + g;
}
