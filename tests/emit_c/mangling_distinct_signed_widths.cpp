// EXPECT: 11
// Distinction rule: integer-width — N4659 §16.3 [over.match].
// f(int) and f(long) are distinct overloads.

int f(int x)  { return x + 1; }
int f(long x) { return x + 7; }

int main() {
    int  a = 1;
    long b = 2;
    return f(a) + f(b);   // 2 + 9 = 11
}
