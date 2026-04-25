// EXPECT: 21
// Distinction rule: reference vs pointer parameter — N4659
// §11.3.2 [dcl.ref]. f(int&) and f(int*) are distinct overloads,
// even though sea-front lowers both to T* in C.

int f(int &r) { return r + 1; }
int f(int *p) { return *p + 10; }

int main() {
    int x = 5;
    return f(x) + f(&x);   // 6 + 15 = 21
}
