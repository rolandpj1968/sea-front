// EXPECT: 25
// Distinction rule: pointee-cv — N4659 §7.3 [conv]. f(int*) and
// f(const int*) are distinct overloads (a const int can't bind to
// int*).

int f(int *p)       { return *p + 1; }
int f(const int *p) { return *p + 7; }

int main() {
    int x = 10;
    const int y = 7;
    return f(&x) + f(&y);   // 11 + 14 = 25
}
