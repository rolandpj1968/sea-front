int main() {
    int a = 1;
    int b = 2;
    int c = a > b ? a : b;
    a += 10;
    b -= 5;
    a *= 2;
    b /= 3;
    a %= 7;
    a <<= 1;
    b >>= 2;
    a &= 0xFF;
    b |= 0x80;
    a ^= b;
    int x = (1, 2, 3);
    return c;
}
