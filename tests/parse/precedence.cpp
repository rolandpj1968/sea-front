int main() {
    int a = 1 + 2 * 3;
    int b = (1 + 2) * 3;
    int c = 1 << 2 + 3;
    int d = 1 & 2 | 3 ^ 4;
    int e = 1 && 2 || 3;
    int f = 1 > 2 ? 3 : 4;
    int g = 1;
    g += 2;
    g *= 3;
    return a + b + c + d + e + f + g;
}
